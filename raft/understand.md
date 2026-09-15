# raft 模块架构理解与下一步规划

> 本文基于对 `raft/` 目录源码的逐文件阅读（`git HEAD c2b9532`），并结合
> `raft/DESIGN.md`、`raft/README.md` 与 `server/raft_bootstrap.{h,cpp}`
> 整理而成。它记录的是**我对现状的理解**，不是对现有文档的复述；
> 若与 DESIGN.md 冲突，以代码为准。

## 1. 模块定位与整体分层

这个 raft 模块的目标是给 sqldb 加复制能力：数据按 key range 切成多个
raft group，每组独立选主、独立复制，SQL 层尽量不动。**切口选在
`kv::KVStore` / `kv::KVEngine`**，复制单位是 `kv::WriteBatch`
（决定 A：不复制 SQL，直接复制一批确定的 put / remove / remove_range）。

整体分四层：

```
┌─────────────────────────────────────────────────────────────┐
│ SQL 层（session / statement / planner / executor）            │  ← 不动
├─────────────────────────────────────────────────────────────┤
│ RaftKVStore / RaftKVEngine（P1 适配层，SQL 看到的 KV 实现）    │
│   - 写 → propose；读 → read-index + 本地已 apply 状态          │
│   - 组内写槽、事务 TxBuffer、client id / request id 幂等       │
├─────────────────────────────────────────────────────────────┤
│ RaftRuntime（单服务线程）── 唯一能碰 RaftNode 的地方            │
│   - tick / 消息 / propose / read_barrier 全部串行化            │
├─────────────────────────────────────────────────────────────┤
│ RaftNode（单 group 状态机，不开线程不碰 socket）               │
│   - LogStore / StateMachine / Transport 三个抽象             │
│   - LevelDBLogStore / KVStateMachine / RaftTcpTransport      │
└─────────────────────────────────────────────────────────────┘
```

`raft/` 目录自身**不认识 SQL**：`LogEntry.data` 对 raft 核心只是一个不透明
字节串，只有 `KVStateMachine` 知道里面是序列化的 `WriteBatch`。

## 2. 核心数据结构（`types.h`）

| 类型 | 说明 |
|---|---|
| `NodeId` | 节点 id，`uint64_t` 包装 |
| `Role` | Follower / Candidate / Leader |
| `LogEntry` | `{index, term, data}`，data 是 proposal 字节串 |
| `HardState` | `{term, voted_for}`，必须落盘后才能 ack |
| `NodeConfig` | 本节点 id、voter 集合（含自己）、选举/心跳间隔 |
| `PeerConfig` | 静态成员 `{node_id, host, port}`，来自 `[raft] peers` |
| `Error` / `ErrorCode` | 全模块用 `std::expected<T, Error>` 传播错误，枚举含 `NotLeader` / `Busy` / `Timeout` 等 |
| `Completion` | 跨线程完成的共享状态：mutex + condition_variable + 结果/错误 |
| `Proposal` | 一次写：index/term/data + completion，`wait()` / `wait_for()` 阻塞等 commit+apply |
| `ReadIndex` | 一次读：目标 commit index + completion，同样的等待语义 |

值得注意：`Proposal` / `ReadIndex` 的 `wait()` 都是**无限期等待**，
调用方必须自己给上限（适配层统一用 `wait_for(election_timeout_ms)`）；
调用方也不能在 raft 服务线程上等，否则死锁（RaftRuntime 用 `Busy` 挡住）。

## 3. 组件逐个理解

### 3.1 `RaftNode` —— 单 group 状态机（核心）

契约：**单线程调用、无线程、无 socket、不依赖 SQL**。生产环境里所有调用
必须发生在 RaftRuntime 的服务线程上。

职责与关键实现：

- `start()`：校验配置（node id 必须出现在 peers、peers 唯一、heartbeat <
  election timeout），从 LogStore 载入 HardState 和日志尾下标，装 transport
  回调，重置选举期限，进入 Follower。
- `tick()`：Leader 到点发心跳（`send_heartbeats`，每轮 `heartbeat_round_` 自增，
  AppendEntries 请求带轮号、响应回显）；非 Leader 到点发起选举；每次 tick 都
  顺手 `apply_committed()`（follower 的兜底 apply 路径）。
- `propose(data)`：仅 Leader。追加一条 `{last_log_index+1, term, data}` 到
  LogStore（先落盘），更新自己的 match/next，立刻 `advance_commit()`，发一轮
  心跳，返回 `Proposal`。单节点时当场 commit + apply 完成。
- `read_barrier()`：仅 Leader。记下请求时刻的 `commit_index` 作为读目标，
  立刻发起一轮心跳，取轮号 `R`；如果多数派已经确认了 `>= R` 的轮次且本地
  apply 已追上目标，立即完成，否则挂进 `pending_read_barriers_` 等
  AppendEntriesResponse 的到达（`evaluate_read_barriers`）。
- 消息处理：`handle_message` 用 `std::visit` 分发到 4 个 handler
  （RequestVote 请求/响应、AppendEntries 请求/响应）。
- 选举：`start_election()` 先 term++、投自己、**save_hard_state 落盘成功后才
  继续**；单成员组直接 `become_leader()`。投票按 Raft 规则：term 更新、
  voted_for 幂等、日志必须 up-to-date（term 优先、其次 index）。
- 成为 Leader：初始化 next/match 进度表，**先追加一条当前 term 的 no-op
  空条目**——这是为了让新 leader 能安全提交上一个 term 遗留的日志。
- 日志复制：`send_append_entries` 从 `next_index` 开始批量带日志；prev_log
  不匹配或缺失则拒绝，leader 收到失败后 `--next_index` 重发（线性回退）。
- commit：`advance_commit()` 取 `match_index` 的中位数作为候选 commit
  位置，**且要求该位置条目的 term 等于当前 term**，然后 apply 并广播心跳。
- apply：`apply_committed()` 从 `last_applied+1` 到 `commit_index` 逐个调
  `StateMachine::apply(entry)`，成功后用返回值完成对应的 pending proposal。
- 领导权丢失 / 日志被截断时：`fail_pending_proposals` 与 `fail_read_barriers`
  把等待者以 `NotLeader` 唤醒，避免调用方挂死。

### 3.2 `LogStore` 与 `LevelDBLogStore`（持久化）

接口：`append / at / truncate_suffix / save_hard_state / load_hard_state /
last_index`。契约是 **append 与 save_hard_state 必须同步落盘成功后才返回**。

`LevelDBLogStore` 实现要点：

- key 布局：`hard_state` 单 key；`log/<8 字节大端 index>`。定宽大端编码让
  LevelDB 的字典序等于日志序号序。
- append / truncate / save_hard_state 全部 `sync = true`。
- 打开时校验日志必须**从 index 1 开始且连续**（P0 无 compaction，不允许空洞），
  这是对“还没做快照/压缩”的显式防线。
- 全类一把互斥锁，内存维护 `last_index_`。

`MemoryLogStore` 只用于测试，不做持久化。

### 3.3 `StateMachine` 与 `KVStateMachine`

接口：`apply(entry)`、`snapshot(range)`、`restore(snapshot)` 以及带 range 的
`restore(range, snapshot)`。

`KVStateMachine` 是生产实现：

- `apply`：空 data 视为 no-op 返回 `"noop"`；否则解码 `ProposalPayload`，
  **先查 `RequestResultStore` 幂等表**——命中直接返回上次结果；未命中把
  WriteBatch 写进本地 KV（`local_->write_batch`，raw 存储路径，绕过事务层），
  成功后把 `(client_id, request_id) → "applied"` 同步落盘。
- `snapshot`：要求有限、非空、正向 range，遍历本地 KV，编码成
  `"SQSN" + version + count + (key,value)*`。
- `restore(range, snapshot)`：校验 magic/version/count，构造一个
  `remove_range + put*` 的 WriteBatch（sync=true）整体写入。

注意：**快照目前只有生成/恢复两端，没有任何触发与传输**——这是最大的未闭合
功能，见 §7。

### 3.4 `ProposalPayload`（日志条目的内容格式）

`"SQRA" + version + client_id + request_id + op_count + ops`，op 含类型
（put / remove / remove_range）、key、可选 value、range_end。所有整数大端、
字节串带长度，二进制安全。sync 标志不编码——持久化策略是本地 LogStore 的事，
不属于复制状态。

### 3.5 `RequestResultStore`（幂等）

接口：`find(client_id, request_id)`、`save(...)`。每个 client 只保留
**最新一条** `(request_id, result)`：

- 相同 request_id → 返回已存结果（重放去重）；
- 已存 request_id 更大 → 拒绝（比最新还旧的请求，防止倒灌）；
- 更小 → 未应用，正常执行。

生产实现 `LevelDBRequestResultStore`：`client/<8 字节 client_id>` → 
`request_id + result`，save 同步落盘，重启后可继续去重。

### 3.6 `Transport` / `message_codec` / `peers`

- `Transport`：`send(to, message)` + `on_message(callback)`。契约：**send 必须
  异步排队，不能同步重入 RaftNode**。
- `Message` 是 `std::variant`（RequestVote 请求/响应、AppendEntries 请求/响应）；
  AppendEntries 携带 `round` 字段用于 read-index 的轮次证明。
- `message_codec`：`u32 长度前缀 + version + type + 字段`，全大端；`FrameDecoder`
  做流式增量解帧，超长帧/坏帧直接判协议违例。不碰 socket，可离线单测。
- `peers`：解析 `1@host:port,2@...`，校验 id/端口/唯一性。

### 3.7 `RaftTcpTransport`（生产传输）

拓扑：**每对节点双向各一条连接**（自己拨每个 peer，接收侧用 TcpServer）。
拨号方先发一帧握手（自己的 node id），接收方因此不需要对端地址。好处是
“每条 fd 只被一条线程拥有”：发送线程独享出站 socket（阻塞 connect+write），
接收线程独享入站 socket（非阻塞读）。

- `send()` 只把编码好的帧塞进出站队列（满了丢帧计数），**独立 sender 线程**
  负责连接与写。
- 写失败：**保留未写字节**，关 fd、退避重连、重发——传输是“至少一次”投递，
  Raft RPC 天然容忍重复。
- 接收路径：先握手，再逐帧 `decode_message`，通过 `post()` 投给 raft 服务
  线程，**绝不内联调 RaftNode**。
- 观测：`sent_frames / dropped_frames / receive_errors / connected_peers`。
- 已知边界：没有连接保活/半开检测（对端僵死只能靠写失败发现），N×(N-1) 条
  连接在 N 大时是运维压力。

### 3.8 `RaftRuntime` 与 `RaftExecutor`

`RaftRuntime` 持有唯一的服务线程（`common::svrkit::ServiceThread`），是
**生产环境中唯一允许碰 RaftNode 的地方**：

- `run(fn)`：阻塞提交，跑完才返回；从服务线程自身调用返回 `Busy`（防死锁）；
  队列满也返回 `Busy`（可重试）。
- `post(fn)`：fire-and-forget，投递失败丢帧计数（transport 用它）。
- `propose` / `read_barrier`：阻塞提交到服务线程，返回的 Proposal / ReadIndex
  仍需调用方 wait_for（完成发生在服务线程）。
- `post_message` / `request_tick`：传输线程 / 定时器线程只能走这两个投递入口。
- `stop()`：先置 stopping，ServiceThread 把队列 drain 完再 join——关停期间
  已入队工作不静默丢失。

`RaftExecutor` 是适配层对 raft 的唯一视图（propose / read_barrier /
node_id / election_timeout_ms / leader_hint）。生产用 `RaftRuntime`，
单线程测试用 `RaftNodeExecutor`（直调 RaftNode，合法前提是调用方自己独占节点）。

### 3.9 `RaftKVStore` / `RaftKVEngine`（SQL 适配层）

`RaftKVStore` 实现 `kv::KVStore`：

- `connect()` 给每个 session 发一条 `RaftKVEngine`，client id 用
  **每进程随机盐 + 自增**（幂等表持久化，纯自增重启后会撞车，盐避免新写被
  误判为重放）。
- `write_batch()` 直接返回 `NotSupported`：raw 写路径只留给状态机 apply
  （状态机直接持有本地 store），SQL 写必须经 propose，否则就是绕过复制的
  本地写后门。
- `new_iterator()` 代理本地已 apply 状态（快照生成等内部用途）。
- `leader_hint()`：本节点不是 leader 时给出 `{node_id, 客户端 SQL 地址}`
  （地址来自 `[raft] sql_endpoints`）。
- 组内写槽：`acquire_write_slot / release_write_slot`，P1 单 group 所以就是
  全 store 一把。

`RaftKVEngine` 实现 `kv::KVEngine`：

- **读**：`ensure_readable()` 先过 `read_barrier()`，用 `election_timeout_ms`
  做等待上限（leader 等不到 quorum 返回 Timeout，follower 返回 NotLeader），
  然后读本地已 apply 状态，**事务内叠加 TxBuffer 覆盖层**（自己写的要看得见），
  iterator 用 `MergingIterator` 合并。
- **写**：`propose_batch()` 把 WriteBatch 编码成 `ProposalPayload`，propose 后
  `wait_for(election_timeout_ms)`。**超时 = 结果未知**（条目可能稍后提交），
  重试必须复用同一个 `(client_id, request_id)`，靠状态机去重。
- **事务**：`begin_transaction` 在本地 store 取快照（可重复读）；第一次写抢
  组内写槽，抢到即释放本地快照（写槽保证没人能提交，缓冲即冻结视图）；COMMIT
  把 TxBuffer 整体编码成**一条** proposal；失败保持事务打开可回滚。
- 错误映射：raft Error → kv::Status（NotLeader / Busy / Timeout / IOError…）。

### 3.10 `server/raft_bootstrap`（启动接线）

组装顺序（不能反）：

```
本地 KVStore（状态机存储，调用方先打开）
  → LevelDBLogStore（<log_path>/log）
  → LevelDBRequestResultStore（<log_path>/request_results）
  → KVStateMachine
  → RaftTcpTransport（sender 线程在此启动）
  → RaftNode
  → RaftRuntime（唯一碰 RaftNode 的线程）
  → listen + inbound 线程 + 心跳定时器线程
  → RaftKVStore（SQL server 看到的接口）
```

关键点：`RaftNode::start()`（装 transport 回调）必须**在 inbound 线程启动前**
于服务线程上完成，否则第一批消息会因回调未装而丢失；关闭顺序严格反过来
（定时器 → transport → runtime → stores）。

## 4. 关键路径速览

### 选举

```
tick 到 deadline → Candidate：term++、投自己、save_hard_state 落盘
  → 单节点直接 become_leader；否则广播 RequestVote
  → 收多数派 vote_granted → become_leader
  → 追加当前 term no-op → 广播心跳 → advance_commit
```

任何 term 更大的消息都触发 `become_follower`（更新 HardState 并落盘）；
曾是 leader 时挂起的 proposal / read barrier 全部以 NotLeader 唤醒。

### 日志复制与 commit

```
propose → 本地 append（sync）→ 更新 self match/next → advance_commit
  → 心跳带新日志给所有 peer
  → peer 校验 prev_log 一致 → 追加/截断冲突后缀 → 回 match_index
  → leader 取 match_index 中位数 → 条目 term == 当前 term → commit_index 前进
  → apply → 完成 proposal → 广播新 commit_index
```

### 读（ReadIndex）

```
read_barrier：目标 = 当前 commit_index
  → 发一轮心跳（轮号 R 自增）
  → 等多数派确认轮次 >= R 且本地 applied >= 目标
  → 完成后读本地已 apply 状态
```

轮号回显的设计是防 stale read 的关键：被分区的旧 leader 没有新轮次回执，
复用历史 ack 或只等 no-op 提交都会放过过期读。

### 写（SQL 视角）

```
写语句/COMMIT → WriteBatch → 抢组内写槽（Busy 则失败）
  → leader？不是则 NotLeader(+hint)
  → 编码 ProposalPayload(client_id, request_id, batch) → propose
  → wait_for(election_timeout_ms)
  → 提交并本地 apply 后返回 OK；超时按“未知结果”处理（重试带同 id）
```

## 5. 我理解的核心设计决策（为什么这么写）

1. **RaftNode 单线程、无锁**：所有状态变更串行化，正确性论证简单；代价是
   并发靠 RaftRuntime 排队，apply（可能很慢的 remove_range）也在服务线程上，
   会挡心跳。
2. **阻塞提交带 Busy**：服务线程自己调用阻塞提交直接拒绝而不是死锁；
   队列满返回 Busy 让调用方可重试。
3. **所有跨线程等待有上限**：被分区旧 leader 不会自己降级，无超时等待等于
   挂死 session；适配层用 election timeout 当预算。
4. **幂等落在状态机**：proposal 超时后结果未知，重放靠
   `(client_id, request_id)` 去重；client id 带进程盐保证重启后不撞。
5. **ReadIndex 轮号证明**：回执只对“不早于读请求发起的那一轮”计数。
6. **no-op 条目**：让新 leader 能提交上一 term 的日志。
7. **LogStore sync 落盘先于 ack**：term/vote/日志不落盘就 ack 会在断电后
   造成选主不稳或丢已确认写。
8. **Transport 至少一次投递 + 每个方向一条连接**：容忍重复、杜绝跨线程写
   socket；握手解决“不知道谁拨进来的”。
9. **复制单位是 WriteBatch 而非 SQL**：apply 时不需要 planner/executor，
   状态机就是 KV 引擎。

## 6. 读代码时的观察

### 优点

- 层次干净：raft 核心不认识 SQL、不碰 socket；接口小且职责单一。
- 错误处理统一走 `std::expected`，边界路径（配置非法、日志不连续、帧越界、
  状态机 apply 失败）都有显式错误而不是静默吞掉。
- 测试设施齐全：可注入假时钟、内存 LogStore、in-proc TestTransport，加上
  真 TCP 集群测试，覆盖了正常路径与分区/超时等故障路径。
- 运维可观测点（丢帧/错误计数）一开始就留了。

### 风险 / 待改进（按我阅读时感受到的优先级）

1. **快照只有“两头”，没有“中间”**：`snapshot()/restore()` 已实现，但没有
   触发策略、没有 InstallSnapshot 传输、没有安装期间的日志暂存/重放、没有
   “日志从快照起点截断”的持久化。因此日志只能无限增长，落后 follower 只能
   靠全量日志追赶。这是当前最实质的正确性/容量缺口。
2. **apply 在 raft 服务线程上**：一次慢的 `remove_range` 会挡住同组心跳与
   propose；DESIGN 里也把它列为未决。短期可接受，长期要么 apply 独立线程，
   要么至少分开“快路径/慢路径”并监控。
3. **读放大**：每次 KV 读都发起一整轮心跳往返（read barrier），尚未合并；
   同语句内多次读（`get_batch` 内部 per-key 各建一个 iterator）也有优化空间。
4. **结构化错误仍偏薄**：`NotLeader` 的 hint 已落地，但
   `CrossGroupTransaction`（P2 需要）还只有状态码；schema 读路径在三态
   （不存在 / 读失败 / 未学到）上也还没修。
5. **运维边界**：无连接保活/半开检测；proposal 等待预算硬编码为 election
   timeout，没有独立 `raft.proposal_timeout_ms`；读超时与写超时未区分。
6. **apply_result 是常量 `"applied"`**：真实 affected rows 一旦要做，必须与
   request result 原子落盘，否则重启重放会返回不一致结果。

## 7. 下一步规划

按“先补正确性缺口、再动性能、最后上多 group 大特性”的顺序排。每阶段的验收
标准都可测试、可回滚。

### Phase A：快照安装 + 日志 compaction（✅ v1 已落地，2026-09-15）

目标：让日志可以截断、让落后 follower 能靠快照追赶，闭合 §6 的第 1 条。
当前落地范围与设计取舍：

1. ✅ `LogStore` 新增 `SnapshotMetadata` 与 `install_snapshot()`：LevelDB 侧
   `snapshot_meta` key 持久化 `last_included_index/term`，打开时允许日志从
   `last_included+1` 开始、自动清理崩溃残留的旧前缀；`at()` 在压缩边界返回
   合成条目，边界之下报错；append/truncate 与压缩共存（Memory 与 LevelDB
   两套实现均已改）。
2. ✅ 新增 `InstallSnapshot` RPC（消息类型 5/6 + codec + TCP 直通），
   `AppendEntriesResponse` 增加失败时的 `hint_last_index`，leader 据此判断
   follower 是否整体落后于压缩点、直接发快照而不是全量回放；
   **分片传输**：快照按 1 MiB/帧切成有序 chunk（offset + done），follower
   在服务线程上按序累积，末帧到达才原子安装；乱序/重叠 chunk 一律返回失败
   由 leader 整体重发（至少一次语义，幂等）。单帧 64 MiB 上限不再是瓶颈。
3. ✅ 触发策略：`[raft] snapshot_entries`（默认 0 = 关闭），**leader 和
   up-to-date follower** 都在 tick 里当“日志条数 - 已压缩条数 ≥ 阈值”时以
   `last_applied_` 为压缩点，截断日志 + 持久化元数据；快照 blob 不在压缩时
   生成（避免 follower 为压缩付全库扫描），只在 `send_snapshot` 需要时按
   **当前 applied index** 生成并打标，保证 blob 与元数据一致。
4. ✅ 安装路径：follower 先 `restore(range, data)`（状态机数据），再
   `install_snapshot()`（日志+元数据）；整次安装原子地跑在 raft 服务线程上，
   同连接按序到达，所以 P0 的“边收边追”暂存在本实现里天然不需要；崩溃窗口
   （restore 后、压缩前）靠幂等 apply 兜底。**防御**：标签低于本节点已 apply
   位置的快照会被显式拒绝（否则恢复会回退状态机、去重又跳过重放，造成静默
   不一致）——正确 leader 不会发这种快照，拒绝只是让调用方可见地重试。
5. ✅ leader 侧 `send_append_entries` 发现 `next_index <= last_included` 时
   改发快照，发完乐观推进 `next/match`。

**已知边界（后续）**：快照生成在 raft 服务线程上扫描全库，慢时挡心跳；
follower 端有 1 GiB 待组装缓冲上限，超大快照仍需要后续做流式落盘。

验收（已通过）：单节点压缩后继续写 + 重启恢复压缩点；三节点 leader 压缩后
新 follower 靠快照追平并继续复制；**2.5 MiB 大快照跨多个 chunk 完整安装**；
**四节点真实 TCP 上晚加入 follower 靠分片快照追平并继续复制**；LevelDB 重启后
快照元数据与日志前缀恢复正确；**up-to-date follower 各自本地压缩日志且继续
复制**；`KVStateMachine` 全 key space（无上界）快照生成/恢复。

### Phase B：读路径与等待收敛（✅ 已落地，2026-09-15）

1. ✅ **read-index 合并（安全版本）**：把 barrier 移到 `begin_transaction`——
   BEGIN 时先过一次 ReadIndex 再取本地快照，事务持快照阶段的每次读都复用这
   一次证明（快照是固定的，重复 barrier 既不刷新数据也不增加正确性，纯浪费）。
   第一次写释放快照后恢复“每次读一个 barrier”（与旧行为一致）。
   **为什么不做“每条语句一次”的更大合并**：那需要 lease（时钟假设），会破坏
   “分区旧 leader 不能放行读”的保证（`IsolatedLeaderCannotServeReads`）；
   事务级合并是当前语义下唯一无损的合并点。自动提交语句 / 多表扫描的读放大
   留给 lease 方案（§11）。
2. ✅ `raft.proposal_timeout_ms` / `raft.read_timeout_ms` 独立配置
   （0 = 回退 election timeout）：`RaftExecutor` 提供默认回退，`RaftRuntime`
   可配置并接线，读/写等待预算分离，分区场景下超时仍受控。
3. ✅ `get_batch` 复用单个 iterator：先解析事务覆盖层（值/墓碑），再用一个
   iterator 逐个 seek 落空 key，避免每个 key 新建+注册迭代器（含
   MergingIterator）；稀疏 key 集仍是点查复杂度，不会退化成区间扫。
4. ✅ **apply 慢路径评估**：给 `RaftNode` 加了 apply 计数与耗时统计
   （`apply_count / total_apply_ns / max_apply_ns`）作监控钩子。结论：
   **暂不把 apply 移出服务线程**——那需要把 last_applied / pending 变成线程
   安全（破坏 RaftNode 无锁不变量），接近重写。真正的先手是让 `remove_range`
   变便宜（LevelDB 当前展开成逐键删：升级用 DeleteRange，或限制单条语句
   删除量）；只有单条语句 apply 依然慢，再考虑独立 apply 线程。

验收（已通过）：事务内多次读只产生一次 barrier（读 barrier 计数验证）；
follower `BEGIN` 立即 NotLeader；`get_batch` 覆盖 store/覆盖层/缺失/重复 key
且两种缺失策略语义不变；超时预算在运行时与配置层都可见。

### Phase C：多 group（P2）

这是 DESIGN 里最大的未开始项：

1. 路由表 `key → group`：P1 先做静态 range 表，**写死 `@system/*` 落 0 号组**；
2. `MultiRaft`（或等价管理类）持有 N 个 `RaftNode`，`RaftKVStore` 从直接持有
   单节点改为路由；
3. 写槽从“store 一把”拆成 per-group（§3.9 的 acquire/release 下沉到组）；
4. 写事务跨组拒绝、跨组只读 `strict` / `loose` 模式 + `CLIENT_OPTIONS` 帧；
   强制点在 `RaftKVEngine`，语句执行前报错，事务保持可回滚；
5. 结构化错误：`CrossGroupTransaction` 带两个 group id；schema 读三态化。

验收（DESIGN §9 P2 的验收标准）：两表并发写互不阻塞；写事务跨组报明确错误；
`loose` 下 `BEGIN; SELECT a; SELECT b; COMMIT` 可跑，`strict` 下报错。

### Phase D：成员变更与长期项（P3+）

- 成员变更：先做单节点增删 + joint consensus（两阶段），配置变更本身走
  proposal（需要特殊 log entry 类型或扩展 payload）；
- follower read / lease：取决于读放大是否成为瓶颈（与 Phase B 联动评估）；
- 表级 split/merge 与 `_meta` group 管理 placement（依赖 C 落地后的路由表
  可改写）；
- 运维：连接保活/半开检测、按组指标、affinity 与告警。

### 快速收尾清单（低风险小项，可随时做）

- `get_batch` 内部 iterator 复用；
- proposal / read 超时预算从硬编码 election timeout 提为可配置；
- ~~LevelDBLogStore“日志从 1 开始”假设~~：已在 Phase A 放宽为
  “从 last_included+1 开始”并补了重启/压缩前缀清理测试；
- 给 `RaftTcpTransport` 补半开检测（周期性写心跳探测或 SO_KEEPALIVE 配置化）。

## 8. 相关文件

- 设计总稿：`raft/DESIGN.md`（含 P0–P3 分阶段计划与 20 条坑）
- 接口/契约与踩坑速查：`raft/README.md`
- 核心状态机：`raft/raft_node.{h,cpp}`
- 单线程宿主：`raft/raft_runtime.{h,cpp}`
- 传输：`raft/tcp_transport.{h,cpp}`、`raft/message_codec.{h,cpp}`
- 持久化：`raft/leveldb_log_store.{h,cpp}`、`raft/leveldb_request_result_store.{h,cpp}`
- 状态机桥接：`raft/kv_state_machine.{h,cpp}`、`raft/proposal_payload.{h,cpp}`
- SQL 适配：`raft/raft_kv_store.{h,cpp}`
- 服务接线：`server/raft_bootstrap.{h,cpp}`
