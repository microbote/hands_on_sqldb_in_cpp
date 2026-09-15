# multi-raft 落地规划（下一步方案）

日期：2026-09-15
输入：`tests/test_raft/multi_raft_assessment.md` 的评估结论 —— 语义内核合格、
生产部署仍是单组；要落地还差传输复用、server 多组启动、按语句路由、路由表
校验四件事。
约束：本文件只做规划，不改代码。

## 0. 目标与范围

把 `raft/` 从"多组语义内核"变成"能跑的多 raft 集群"：

- K 个 group 共享同一节点集合（沿用现有单 `[raft] peers` 表），每组独立
  选主/复制/commit/apply；
- SQL 层按语句路由：写语句/DDL 定位到目标组，跨组写事务拒绝，
  strict/loose 读规则生效，NotLeader 带**目标组**的 leader hint 重定向；
- 单组部署路径（现在的行为）保持可用，作为 K=1 的退化情形。

三个里程碑：

| 里程碑 | 内容 | 性质 |
|---|---|---|
| M1 | 静态分片可跑通（传输复用 + 多组启动 + 按语句路由） | 本期目标 |
| M2 | 按表自动分配 group（建表时决定归属） | 后续 |
| M3 | 每 group 独立快照（loose = N 份 per-group 快照） | P2b，存储层 |

明确不做：成员变更（P3）、表级 split/merge、lease、全局一致快照、follower read。

## 1. 关键决策（先定方向再动手）

### D1 传输复用：共享 transport + 消息带 group id（推荐）

两个候选：

| 方案 | 做法 | 代价 |
|---|---|---|
| A：每 group 独立 transport | 每组一个监听端口 + 一组 peers | 线格式零改动，但端口 ×K、连接数 ×K（K×N×(N-1)）、配置膨胀；与 DESIGN "N 节点 = N×(N-1) 连接、规模小" 的意图冲突 |
| B：共享 transport + group id | 一条连接承载所有组；raft 消息 payload 在 version 后加 `u64 group_id`（`kMessageVersion` → 3）；接收端按 group 分发 | 需要改 codec、Transport 接口、RaftNode 发送点、TestTransport，波及现有测试（机械适配） |

**选 B**。理由：

1. 所有组共享同一成员表 —— 现有单 `peers` 配置正好支持，不用为每组发明
   新地址体系；
2. 端口/连接数不随组数增长，"每条 fd 单线程拥有"不变量不变；
3. 接收端只是多一张 `group_id → 回调` 表（`on_message(group_id, cb)` 按组
   注册），RaftNode::start() 的注册动作形态不变。

改动面（S1 细化为任务清单）：

- `raft/message_codec.{h,cpp}`：payload 布局 `version, group_id(u64), type,
  fields`；`decode_message` 返回 `{group_id, Message}`；版本号 +1；
  帧长度前缀与分帧不变（握手帧不带 group id，仍走通用分帧）。
- `raft/transport.h`：`send(NodeId to, uint64_t group_id, const Message&)`；
  `on_message(uint64_t group_id, cb)` 按组注册；测试用 in-proc 传输同步改。
- `raft/types.h`：`NodeConfig` 增加 `uint64_t group_id`（默认 0，单组不感知）。
- `raft/raft_node.cpp`：所有 `transport_.send(...)` 带 `config_.group_id`；
  `start()` 用 `config_.group_id` 注册回调。
- `raft/tcp_transport.{h,cpp}`：出站帧带 group_id；入站解出后查表分发；
  发往同一 peer 的队列仍是一条（先不做 per-group 队列，M1 观察背压）。
- 测试：codec 往返（含 group_id）；in-proc 共享 transport 两组消息不串组；
  真实 TCP 一条连接上两组流量各自到达正确的 RaftNode。

### D2 分片配置：静态 range（M1 用，M2 换成表级分配）

配置格式（二选一，倾向后者）：

- DESIGN 原案 `shard.N = <start>,<end>`（N 即 group id）；
- 或 `[raft] shards = <start>:<end>:<group_id>,...` —— 单键多段、解析简单。

校验（`GroupRouter::validate()` 或 config 层）：

- range 两两不相交；
- `@system/*` 不允许被数据 range 覆盖（0 号组专属）；
- group_id 从 1 起、不重复；
- 未覆盖的 `@data/*` 键落哪个组必须语义明确（默认组 0 或拒绝，二选一，
  推荐拒绝并让运维补全 range）。

注意：静态 range 是按 key 字节范围切，运维要懂 `@data/<db>/<table>/` 的编码
才能保证"一表一组"——这正是 M2 换表级分配的原因，M1 接受这个运维成本。

### D3 每 group 独立持久化

- Raft 日志与 request-results 每 group 独立子目录：
  `<log_path>/group<N>/log`、`<log_path>/group<N>/request_results`。
  理由：`LevelDBLogStore` / `LevelDBRequestResultStore` 零改动，组间无 key
  耦合，每组自包含、可独立删。
- 状态机存储（业务 KV）仍是所有组共享一份（现有设计不变）。

### D4 server 按语句路由

- `RaftKVStore` 增加 `group_for(const kv::Key&)` 与
  `leader_hint_for(const kv::Key&)`（按 key 返回目标组的 hint；DDL/USE 的
  `@system/*` 键 → 组 0）。
- session 写槽改为按组：写语句执行前由语句目标算出组，再
  `acquire_write_slot(group)`（替代现在无参默认抢 0 号组的做法，修掉评估里
  指出的"跨组写会误导性失败"问题）。
- server 预检查 `execute_on_this_node` 按语句目标组检查 hint：语句目标表 →
  表 key 前缀（`@data/<db>/<table>/`）→ `leader_hint_for`；不是该组 leader
  就回 NotLeader + 该组 leader 地址。
- 需要 session/executor 提供"语句目标表/目标键"：解析后的 Query 已知
  db/table（写语句、DDL、SELECT 都拿得到）；多表单语句不存在（语法层保证）。

## 2. 里程碑与任务

### M1：静态分片可跑通

| 步骤 | 内容 | 主要产出 |
|---|---|---|
| S1 | 传输复用（D1） | codec/Transport/RaftNode/TestTransport 改动 + 三组测试（codec 往返、in-proc 不串组、TCP 一组连接两组流量） —— **✅ 已落地 2026-09-15** |
| S2 | shard 配置解析 + 路由校验（D2） | `[raft] shards` 解析与校验 + `GroupRouter::validate()` + 配置/路由测试 |
| S3 | bootstrap 多组启动（D3） | K 组 = K×(LogStore+KVStateMachine+RaftNode+RaftRuntime)，共享一个 transport + 一个 RaftKVStore；K=1 走现有路径 |
| S4 | server 按语句路由（D4） | `group_for/leader_hint_for`、session 写槽按组、预检查按组 |
| S5 | 端到端验收 | K=2~3、N=3 真实 TCP：每组独立 leader；组 A 写不影响组 B；跨组写事务报错带组号；follower 组回 NotLeader 指向该组 leader；loose 跨组读可跑 |

测试矩阵（M1）：

- 单元：codec group_id 往返；`GroupRouter::validate`（重叠/覆盖 @system/重复
  id/缺段）；shard 配置解析；
- 适配层（in-proc，多组 × 单节点，扩展现有 `test_raft_multi_group`）：路由、
  per-group 写槽、跨组规则、`leader_hint_for`；
- 传输（真实 TCP）：一条连接上两组消息各自到达正确 RaftNode；
- 端到端（真实 TCP，多组 × 多节点，新测试文件）：上面的 S5 验收项。

### M2：按表自动分配（后续）

- `CREATE TABLE` 时分配 group（`hash(db,table)` 或 round-robin），把
  `(db,table) → group` 记录到 `@system` 映射表（组 0）；
- 路由表从静态 range 换成"表前缀精确匹配"：`@data/<db>/<table>/` 前缀 →
  group；`@system/*` 仍固定组 0；
- `DROP TABLE` 回收；映射表读路径要过组 0 的 barrier；
- 路由改为"查表 + 缓存"，需要处理表还没建（路由未知）时的错误语义。

### M3：每 group 独立快照（P2b，存储层）

- loose 事务 = 首次读某组时取该组快照（N 份 per-group 快照），替换现在
  "BEGIN 全组 barrier + 全局快照" 的近似；
- 难点：LevelDB 快照是全局的，没有按 range 快照；要么给本地存储加
  "按 group 前缀分离" 的视图，要么评估能否接受全局快照近似（M1 先接受，
  M3 再决定是否值得做）。

## 3. 验收标准（M1）

1. K=3、N=3 真实 TCP：每个 group 选出**自己的** leader（互不干扰）；
2. 组 A 的写提交后，组 B 的读写不受影响（per-group 写槽与 barrier 独立）；
3. `BEGIN; 写组A表; 写组B表; COMMIT` 在语句层报错且带
   `(group X -> group Y)`；
4. 连接打到组 B 的 follower：写/读返回 NotLeader，且 hint 指向**组 B** 的
   leader（不是组 A/0 的）；
5. `--cross-group-read=loose` 下 `BEGIN; SELECT 组A表; SELECT 组B表; COMMIT`
   可跑；strict（默认）报错；
6. 单组部署（不配 shards）行为与现在完全一致（现有全部测试不回归）。

## 4. 明确不做（本期）

- 成员变更 / joint consensus（P3）；
- 表级 split/merge（P3）；
- lease / follower read（P3）；
- 全局一致快照 / 全局时间戳（明确不在设计内）；
- per-group 发送队列（M1 先观察共享队列背压，有问题再拆）。

## 5. 风险

1. **传输接口改动波及所有 raft 测试**：一次性机械适配成本，S1 单独做、
   全量回归后再进 S2。
2. **静态分片配置对运维不友好**：要懂 key 编码才能配出"一表一组"；M2
   换表级分配后消失，M1 接受。
3. **全局快照近似**：M1 语义上比 DESIGN 的 per-group 快照更强（更一致），
   但钉住整库旧版本；M3 再收紧。
4. **共享 transport 背压**：一组慢 apply 可能堵住公共发送队列；M1 加
   观测（发送/丢弃计数已有），不预先拆队列。
5. **session 写槽按组**是行为变更点：路由必须先于抢槽，顺序做错会重现
   评估里"跨组写误导性失败"的问题，S4 用专门的测试钉住。

## 6. 顺序与建议

- 按 S1 → S2 → S3 → S4 → S5 顺序执行；S1 是最大块（线格式 + 接口 + 测试
  适配），先行并全量回归；
- S2/S3/S4 各自独立，可在一个执行轮次里连续完成；
- 每步的验收都落测试，最终 S5 的端到端用例是新文件
  `tests/test_raft/test_raft_tcp_multi_group.cpp`（多组 × 多节点真实 TCP）。
