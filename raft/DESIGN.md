# raft 模块设计：sqldb 的 Multi-Raft

English version: [DESIGN.en.md](DESIGN.en.md)

**状态：设计已定稿；P0 core 已落地（选举 / 日志复制 / commit / apply /
LevelDB 日志持久化 / proposal payload 编解码 / KV 状态机桥接）。**
本文件是实施方案，不是使用说明；职责/接口/踩坑见 `raft/README.md`。

## 0. 目标与范围

目标：给 sqldb 加 **multi-raft** —— 数据按 key range 切成多个 raft group，
每组各自选主、各自复制；**复用现有网络/协程框架**，SQL 层尽量不动。

两条已拍板的决定（下面的设计全部围绕它们展开）：

| # | 决定 | 影响 |
|---|------|------|
| **A** | **直接复制 KV `WriteBatch`**（不是复制 SQL、不是复制条件命令） | 状态机是 KV 引擎，planner/executor 不搬进 apply |
| **B** | **禁止跨 group 的写事务**；**跨组只读**由客户端模式 `strict` / `loose` 决定（见 §3.3） | 写事务被钉在单个 group 内；写并发语义 = 现有"悲观单写者"按 group 缩小 |

**不在范围内**（明确不做，避免范围蔓延）：跨 group 的**写**事务与 2PC、全局锁库、
**全局一致快照（全局时间戳 / MVCC）**、表内 split/merge、成员变更
（joint consensus）、follower read / lease、异地多活。

## 1. 总体架构

```
client ──SQL 协议──► sqldb-server (svrkit::TcpServer)        ← 复用，不改
                          │ Session / statement / planner /
                          │ executor / relation                ← 全不动
                          ▼
                    kv::KVStore / kv::KVEngine                 ← 接口不变，换实现
              ┌───────────┴────────────┐
       RaftKVStore（新）          本地 LevelDB（原样，当状态机）
              │ MultiRaft
              ├─ RaftNode × N（一个 group 一个）
              └─ raft 传输：svrkit::TcpServer + common/net（独立端口）
```

### 1.1 为什么切口选在 `kv::KVStore` / `kv::KVEngine`

1. `Session → statement → relation → kv` 已经是一层干净接口，换实现上层零改动；
2. `WriteBatch` 就是天然的复制单位 —— 一批确定的 `put` / `remove` /
   `remove_range`（决定 A 直接把它当日志条目的 payload）；
3. **key 布局已经替分片准备好了**：`@data/<len db>/<len table>/` 是连续前缀，
   `relation/key_prefix.h` 的 `prefix_end()` 就是右界 —— 于是"**一个表 = 一个
   group**"天然对齐，不需要发明新的分片边界；
4. `KVStore` 本来就是"进程内一份 + `connect()` 给每个 session 一条连接"的形状，
   天然对应"本地状态机 + 多条连接"。

## 2. 分片模型：key → group

### 2.1 分片粒度：**一个表 = 一个 raft group**

| key 前缀 | 归属 |
|----------|------|
| `@data/<len db>/<len table>/<encoded pk>` | 该**表**所在的 group |
| `@system/*`（`@system/databases`、`@system/tables/<db>`、`@system/schema/<db>/<table>`、`@system/tablestats/...`、`@system/dbstats/...`） | **固定 0 号 group（meta group）** |

理由：

- `@data/<db>/<table>/` 是连续前缀，`[prefix, prefix_end(prefix))` 就是一个
  精确的 range —— 分片边界不用另造；
- `DROP TABLE` / `TRUNCATE` 走 `WriteBatch::remove_range`，整段删除**必须落在
  同一个 group 内**；一表一组天然满足；
- 表内切分（把一张表再切几段）会让 `remove_range`、全表扫描、统计信息同时
  跨组，收益远小于复杂度，明确不做。

### 2.2 路由表

`key → group` 是一张**有序的 range 表**：`[start_key, end_key) → group_id`。

- **P1**：静态配置（`shard.N = <start>,<end>`），先只有一个 group；
- **P2**：按表生成，每张表一段；
- **P3**：交给 `_meta` group 管理（届时才有 split/merge 的前提）。

`@system/*` 全部落 0 号 group。**这条必须写死在路由表的最前面**：漏了它就会
得到"`CREATE DATABASE` 只在 leader 上生效、别的节点看不到库"的诡异现象。

## 3. 事务规则（决定 B 的具体形状）

### 3.1 规则

1. **写事务**（`BEGIN` … `COMMIT`，期间发生过任何写）整个生命周期只能落在
   **一个 group**；
2. 事务在**第一条碰到数据的语句**上绑定 group（读也算）。此后：

   | 情况 | 结果 |
   |------|------|
   | 指向同一 group | 照常 |
   | 指向别的 group，且**本事务还没写过** | 进入"跨组只读"，**是否允许看模式**（§3.3）：`strict` 报错，`loose` 放行 |
   | 指向别的 group，且**本事务已经写过** | **立刻报错**（写事务不跨组 = 决定 B） |
   | 已经跨过组（读过 ≥2 个 group），**再想写** | **立刻报错**（一旦跨组就冻结为只读事务） |

3. 上面所有报错都在**语句执行前**给出，事务保持可回滚 —— 不是等到 COMMIT 才发现；
4. 自动提交的单条语句天然只碰一张表（现有语法里没有跨表单语句），因此不受
   影响 —— `SELECT * FROM a; SELECT * FROM b;` 是两条独立语句，照常工作。

### 3.2 写槽（悲观单写者）→ per-group

现有语义（见 `storage/kv_engine/kv_engine.h` 顶部注释）：

- `begin` = 开事务**并取快照**（可重复读）；
- 写槽**全进程唯一**；**拿到写槽时快照被释放**（因为写槽保证没人能提交，
  "最新已提交 + 自己的缓冲"本身就是冻结视图）；
- 第二条连接的**写**拿不到写槽 → `Status::Busy`；
- 只读事务不抢写槽，所以**读者不阻塞写者**。

本设计**原样保留这套语义，只把写槽的作用域从"全进程"缩到"单个 group"**：

- 写槽挂在 group 上（`RaftGroup` 持有），一个 group 同一时刻只有一个写事务；
- 因为事务被钉在单个 group，**"拿到本 group 写槽即可释放快照"这条规则可以
  照搬**（作用域从进程缩到 group）；
- 结果：**不需要读集校验、不需要行锁、不需要 2PC** —— 单组单写者就是
  可串行化的，与今天同一套论证。

> 这是决定 B 的最大收益：**用"禁止跨组"换掉了整套分布式事务**。代价是
> 显式事务不能用多个表 —— 明确接受。

### 3.3 跨组只读事务：`strict` / `loose` 两种模式

**为什么需要模式**：不同 group 的提交时刻不同，跨 group 的"快照"**不是**一个
全局快照，可能观测到一个从未真实存在过的状态。与其替用户决定，不如让客户端
显式选。

| 模式 | 跨组只读事务 | 语义 |
|------|--------------|------|
| `strict`（**默认**） | **拒绝**，明确报错 | 不假装提供全局一致快照 |
| `loose` | 允许 | **每个 group 各自一份快照**（在该 group 被第一次读时取）——**不是全局一致快照** |

几点要说清楚的：

- `strict` 在 P1/P2 就是"拒绝"，**不需要任何额外机制**。真正的"严格全局一致
  快照"要全局时间戳 / MVCC（见 §11），明确不在本设计内；
- `loose` 的成本几乎为零：`begin` 时**不取快照**，第一次读某个 group 时才取
  那个 group 的快照，于是这个事务持有 **N 份 per-group 快照**；
- `loose` 的**资源含义要写进用户文档**：一个长期挂着的 loose 事务会把多个
  group 的 leveldb 旧版本钉住（和现在"事务挂着不动钉住旧版本"是同一个问题，
  只是钉住的地方变多）。`idle_in_transaction_timeout_ms` 对它同样适用；
- `loose` 事务**仍然不占写槽**，读者不阻塞写者；
- **写过就不许再跨组**（规则 §3.1 的最后两行）：否则又变成跨组写事务了。

### 3.4 协议改动：客户端声明 loose / strict

- **默认 `strict`**（安全）；要放宽必须客户端显式声明。
- 传参位置：**连接级**，不是事务级。理由：现有语法层明确拒绝事务模式
  （`sql.y` 里那句 `transaction modes are not supported (use plain BEGIN)`），
  连接级设置**不用动 SQL 语法**，CLI 一个开关就能表达；而且同一个连接里不同
  事务用不同模式，"可重复读"更难讲清楚。
- 协议现状：`HELLO` 只有 server→client（`u16 proto_version, u16 server_version,
  u32 capabilities`），**没有 client→server 的选项通道**。需要新增：

  ```
  CLIENT_OPTIONS  client->server（HELLO 之后发一次）
    u32 capabilities          // 位；新增 kCrossGroupReadLoose
    u16 option_count
    { u16 key_len, bytes key, u16 value_len, bytes value } * option_count
  ```

  可扩展的 key/value，加选项不用再改帧格式。**能力位协商**：server 在 HELLO
  里通告支持 `kCrossGroupReadLoose`，客户端看到才发；看不到就保持 strict。
  老 server 读到未知帧按协议错处理（断开），老 client 不发 → 默认 strict，
  两边都不会静默跑错语义。
- CLI：`sqldb-client --cross-group-read=loose|strict`（本地 `sqldb` 同款开关）。
- 落点：`session::Session` 持有模式；**强制点在 `RaftKVEngine`** —— 它看得见
  每个 key/range、也知道路由表，顺手就能记住"本事务已经碰过哪些 group"，
  违约返回 `Status::CrossGroupTransaction`，server 转成 ERROR 帧。

### 3.5 并发收益

不同 group 的写可以**并行**（各自 leader、各自写槽）。这是相对现在"全进程
单写者"的实质提升，也是做 multi-raft 的根本理由。

## 4. 组件与接口

### 4.1 `raft/`（不认识 SQL）

```cpp
namespace raft {

struct NodeId { uint64_t value; };

struct LogEntry {
  uint64_t index;
  uint64_t term;
  std::string data;      // 决定 A：这里就是序列化后的 kv::WriteBatch
};

struct HardState {       // term/vote 必须落盘后才能 ack
  uint64_t term = 0;
  std::optional<NodeId> voted_for;
};

// 持久化：term/vote + 日志段。可以单独开一个 LevelDB（推荐，和业务数据解耦）。
class LogStore {
  virtual std::expected<void, Error> append(const LogEntry &);
  virtual std::expected<LogEntry, Error> at(uint64_t index);
  virtual std::expected<void, Error> truncate_suffix(uint64_t from);
  virtual std::expected<void, Error> save_hard_state(const HardState &);
  virtual std::expected<HardState, Error> load_hard_state();
};

// 状态机：SQL 侧实现（把 batch 应用到本地 KV）
class StateMachine {
  virtual std::expected<std::string, Error> apply(const LogEntry &) = 0;
  virtual std::expected<std::string, Error> snapshot(KeyRange) = 0;   // 序列化
  virtual std::expected<void, Error> restore(std::string_view) = 0;
};

// 传输：生产用 svrkit；测试用 in-proc（沙箱禁 bind，必须能脱离 socket 测）
class Transport {
  virtual void send(NodeId to, const Message &) = 0;
  virtual void on_message(std::function<void(NodeId, Message)>) = 0;
};

class RaftNode {                 // 一个 group
  void tick();                   // 时钟驱动：选举/心跳
  std::expected<Proposal, Error> propose(std::string data);  // 只有 leader 成功
  bool is_leader() const;
  Role role() const;             // Follower / Candidate / Leader
  uint64_t term() const;
  uint64_t commit_index() const;
  uint64_t applied_index() const;
  std::optional<NodeId> leader_hint() const;
};

class MultiRaft {                // 路由 + 多组管理
  RaftNode *group_for(const kv::Key &);
  std::expected<Proposal, Error> propose(const kv::WriteBatch &);  // 要求单组
  std::vector<GroupInfo> groups() const;
};

} // namespace raft
```

### 4.2 SQL 侧的新实现

```cpp
class RaftKVStore : public kv::KVStore {
  std::shared_ptr<kv::KVStore> local_;   // 本地 LevelDB，当状态机用
  raft::MultiRaft raft_;
};

class RaftKVEngine : public kv::KVEngine {
  // get / scan            -> 读本地已 apply 的状态（leader 上先过 read-index）
  // write_batch / commit  -> 路由到 group -> propose -> 等 commit -> apply
};
```

新增 `kv::Status`：

- `NotLeader`（配 `leader_hint`）—— server 回 ERROR 帧，客户端重连到 leader；
- `CrossGroupTransaction` —— 事务越界（决定 B），带两个 group id 便于定位。
  两种情况：**写事务跨组**（任何模式都拒绝），或**跨组只读**且模式为 `strict`。
  强制点在 `RaftKVEngine`（它看得见 key 和路由表），不是 SQL 层。

**SQL 协议不需要改**：`NotLeader` 走现有的 ERROR 帧通道。

## 5. 读写路径

### 5.1 写（`COMMIT` / 自动提交语句）

```
statement 执行完 -> WriteBatch（已有）
  -> 路由：batch 内所有 key 必须落同一个 group，否则 CrossGroupTransaction
  -> 持有该 group 写槽？（没有则 Busy）
  -> 本节点是该 group 的 leader？
       否 -> NotLeader(+hint)
       是 -> LogStore.append(entry)（先落盘）
          -> 复制到多数派
          -> commit_index 前进
          -> StateMachine.apply(batch) 应用到本地 LevelDB
          -> 返回 OK（affected_rows 用 apply 的返回值）
```

### 5.2 读

- **P1：读写都走 leader**；
- leader 上的读必须先过 **read-index**：记下请求时刻的 `commit_index`，等
  `applied_index >= 该值` 再读本地 —— 否则刚提交的写可能读不到；
- P3 才考虑 follower read / lease（不在范围内）。

### 5.3 幂等

`put` / `remove` 本身幂等，但"读后决定"的 batch 在**重试时会重新计算**，
不再是同一条。因此日志条目要带 **client request id**，状态机记住每个
client 的最后一个已应用 id，重复的 propose 直接返回上次结果。

## 6. 快照

- follower 落后超过阈值（或日志被压缩）时，leader 发整个 group 的 range；
- 生成：对 `[start_key, end_key)` 做 `scan`（现有 `Iterator` 就能用）；
- 接收：清空该 range 再灌入，**边收边追**（安装期间新来的日志要暂存/重放）；
- `@system/*` 的 meta group 同样要支持快照。

## 7. 配置

新增 `[raft]` section（复用现有 `server::Config` / `validate()` / `Logger`）：

```ini
[raft]
enabled = false
node_id = 1
listen  = 127.0.0.1:5434
peers   = 1@127.0.0.1:5434,2@127.0.0.1:5435,3@127.0.0.1:5436
election_timeout_ms = 1000
heartbeat_ms        = 100
log_path            = ./sql_db_raft_log     # 单独的 LevelDB 存 raft 日志
# 静态分片（P3 之前手写；之后交给 _meta group）
shard.0 = ,+                                 # [空, +∞) = 全部
```

## 8. 复用清单（不重造轮子）

| 需要 | 用什么 |
|------|--------|
| raft 传输（收） | `svrkit::TcpServer` + 自己的 `ConnectionHandler`，**独立监听端口** |
| raft 传输（发） | `common/net::TcpSocket::connect`；**每对 peer 一条长连接**，不要每条 RPC 阻塞 connect |
| raft 核心 / apply 线程 | `ServiceThread` + `SubmitToService`（**状态机绝不放进协程层**） |
| 心跳 / 选举 / 跨线程唤醒 | `Loop::add_timer` / `Loop::post`（时钟要能注入假的，见 §9） |
| 配置 / 日志 / 指标 | `server::Config`、`server::Logger`、`Metrics` 同样形状 |
| 日志条目体 / 快照 / apply | `kv::WriteBatch`、`kv::Iterator`、`scan`、`prefix_end` |

**为什么 raft 用独立端口**：SQL 与 raft 的超时/鉴权/背压完全不同；复用同一
端口需要在 SQL 协议上做"首字节分流"，会把 SQL 帧解析器污染成内部流量的搬运工。

## 9. 分阶段计划

| 阶段 | 内容 | 验证方式 |
|------|------|----------|
| **P0** | raft 核心：选举 / 日志复制 / 提交 / apply / 快照；**in-proc transport + 可注入假时钟** | 单测：正常路径、分区、丢消息、乱序、重启、单节点→三节点。**沙箱内可全绿**（不碰 socket） |
| **P1** | 单 group 打通 SQL：`RaftKVStore` + 静态 placement（1 节点 → 3 节点）+ leader-only 读写 + read-index + 幂等 id | `test_raft` + 端到端：SQL 跑在 raft 上，杀掉 leader 后能重新选主并继续 |
| **P2** | 多 group：按表分片；`@system/*` 落 0 号组；每 group 独立 leader；**写事务跨组拒绝 + `strict`/`loose` 模式 + `CLIENT_OPTIONS` 帧**（§3.3/§3.4） | 两表并发写互不阻塞；写事务跨组报明确错误；`--cross-group-read=loose` 下 `BEGIN; SELECT a; SELECT b; COMMIT` 能跑，`strict`（默认）下报错 |
| **P3** | `_meta` group 管 placement、**表级 split/merge**、成员变更、follower read / lease | 需另行设计（本文档不含） |

**从 P0 开始**：它不碰 SQL、能确定性测试、沙箱里也能全绿，而且 P0 定下的
`LogStore` / `StateMachine` / `Transport` 三个接口决定 P1 顺不顺。

## 10. 坑（按危险程度）

1. **`@system/*` 也必须复制**。`CREATE DATABASE/TABLE` 写的是 meta key，不是
   `@data/*`。忘了这条 = "schema 只在一个节点上"。固定落 0 号 group。
2. **全局写槽必须拆成 per-group**。`KVStore::write_slot_held()` 现在是全进程
   一把；不拆，多 group 拿不到并行（等于白做）。
3. **快照释放的规则要跟着作用域走**：现在是"拿到全进程写槽就释放快照"，
   改成"拿到本 group 写槽就释放本 group 的快照"。这条如果照抄成"拿任何写槽
   就释放全部快照"，会破坏可重复读。
4. **term/vote 与日志必须落盘后再 ack**，否则断电后可能选主不稳/丢已确认写；
   要和现有 `WriteBatch::set_sync(true)` 的语义对齐。
5. **幂等**：重试会重新计算 batch（§5.3），必须有 client request id 去重。
6. **`remove_range` 必须落在同一 group 内** —— 这是"一表一组"的另一个理由。
7. **时钟要能注入假的**：心跳/选举最终用 `Loop::add_timer`，但 P0 必须能
   脱离真时钟做确定性测试。
8. **不要把 raft 状态机放进协程/Loop 线程** —— 沿用既有结论（见
   `tests/test_server/codex_check_issues.md`）。
9. **`loose` 会同时钉住多个 group 的旧版本**：一个挂着的 loose 事务 = N 份
   per-group 快照。空闲超时对它必须同样生效，用户文档也要写清楚。
10. **跨组检查要在语句执行前做**，且强制点在 `RaftKVEngine`（它才知道 group
    边界）；放到 SQL 层去判断会漏掉 `@system/*` 那张路由表，也会让"读过哪些
    group"这种状态多存一份。

## 11. 未决 / 后续

- **`strict` 的真正实现**：现在 `strict` = 拒绝，不是"提供全局一致快照"。
  要做到名副其实，需要全局时间戳（HLC / 中心授时）+ MVCC（按版本读），
  这是独立的大特性，不在本设计内。届时要重新评估 `strict` 的默认值。
- `loose` 要不要再细分（比如"每个 group 读最新"vs"每 group 快照"）？
  目前只做"每 group 快照"这一种。
- 日志压缩（compaction）策略：按条数还是按字节？快照多久做一次？
- 客户端在收到 `NotLeader` 后**由谁重试**：CLI 直接重连，还是 server 代转？
  （P1 先做简单的：把 leader hint 回给客户端。）
- P3 的 follower read / lease 是否需要，取决于读放大是否成为瓶颈。
- 表级 split/merge 之后，"一个事务只碰一个 group"的规则要重新审视
  （同一个表跨两个 group 就重新变成跨组事务了）。
