# raft 模块设计：sqldb 的 Multi-Raft

English version: [DESIGN.en.md](DESIGN.en.md)

**状态：设计已定稿；P0 core、P1a 适配层、P1b 运行时/传输/接线都已落地**
（选举 / 日志复制 / commit / apply / LevelDB 持久化 / payload 编解码 /
KV 状态机桥接 / `RaftKVStore` + `RaftKVEngine` / ReadIndex 读屏障 / 组内写槽 /
`RaftRuntime` 单线程服务 / RPC 编解码 / TCP transport / `[raft]` 配置与
`sqldb-server` 接线）。**Phase A v1（快照/压缩）已落地**：leader 侧按
`[raft] snapshot_entries` 阈值生成快照并压缩日志，落后/新 follower 通过
`InstallSnapshot` 追赶（当前单帧传输，分片式与 follower 本地压缩留作后续）。
剩余工作见 §11（read-index 合并、成员变更、P2 多 group、分片快照）。
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

class RaftRuntime {              // P1b 已落地：RaftNode 的唯一宿主线程
  void start(size_t queue_max);
  void stop();                   // 先把队列跑完再 join
  std::expected<void, Error> run(std::function<void()>);        // 阻塞提交
  std::expected<Proposal, Error> propose(std::string data);     // 阻塞提交
  std::expected<ReadIndex, Error> read_barrier();               // 阻塞提交
  bool post_message(NodeId, const Message &);                   // 投递
  bool request_tick();                                          // 投递
};

} // namespace raft
```

`message_codec.{h,cpp}` 提供 RPC 的二进制帧（带版本、长度前缀、流式解码器），
`peers.{h,cpp}` 解析 `[raft] peers` 的 `id@host:port` 列表。两者都不碰 socket，
所以在沙箱里可以完整单测。

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

### 4.3 P1 适配层：已实现的契约（`raft/raft_kv_store.{h,cpp}`）

单 group 版本把 §4.2 的草图落成了两个类。**P1 只有一个 group，所以
`RaftKVEngine` 现在直接持有 `RaftNode &`**；P2 才把这一处换成
`MultiRaft::group_for(key)` 路由（见 §11）。

| 类型 | 职责 |
|------|------|
| `RaftKVStore` | 实现 `kv::KVStore`；`connect()` 给每个 session 一条 `RaftKVEngine`，`open/close/flush/stats` 代理本地 store |
| `RaftKVEngine` | 实现 `kv::KVEngine`；读走 read-index + 本地已 apply 状态，写走 propose → 等 commit + apply，显式事务在 `COMMIT` 时整批复制 |

契约（按重要性排序）：

1. **写只能经 `connect()` 进入**。`RaftKVStore::write_batch()` 返回
   `NotSupported`：raw store 写路径是留给状态机 apply 的（它直接持有本地
   store），SQL 写必须走 propose，否则就是绕过复制的本地写。反过来，
   `RaftKVStore::new_iterator()` 读到的是本地已 apply 状态，只读且安全，
   直接代理（快照生成等内部用途需要它）。
2. **读前必须过 read-index**（算法见 §5.2）；不是 leader、或等不到 quorum，
   分别返回 `NotLeader` / `Timeout`。
3. **事务 = 本地快照 + 组内写槽**：`begin_transaction()` 在本地 store 上取
   快照（可重复读）；第一次写时抢本组写槽，抢到就按 §3.2 的规则释放快照；
   `COMMIT` 把 `TxBuffer::to_batch()` 整体编码成**一条** proposal。
   `COMMIT` 失败时事务保持打开、可回滚，本地状态不被污染。
4. **读自己的写**：事务内 `get/exists/get_batch/new_iterator` 先看
   `TxBuffer` 覆盖层，再看本地已 apply 状态 —— 与本地引擎的语义一致。
5. **空事务 `COMMIT` 不产生日志**：没有 op 时直接结束事务。
6. **幂等**：每条 proposal 带 `(client_id, request_id)`；`request_id` 在一条
   连接内单调递增，客户端重试**必须复用同一个 id**，状态机按它去重。
7. **超时 = 结果未知，不是"没写进去"**：`Proposal::wait_for()` 超时只说明
   没等到 commit，该条目仍可能随后被提交。调用方要么用同一个 request id
   重试，要么把不确定暴露给用户；不能假定"报错 = 没生效"。
8. **等待都有上限**：读屏障与 proposal 等待都取
   `election_timeout_ms` 作为预算。分区里的旧 leader 不会因为选不出新主而
   自己降级，没有上限就会把 session 挂死（`IsolatedLeaderCannotServeReads`
   与 `IsolatedLeaderFailsReadsAndWritesWithTimeout` 覆盖这两条路径）。

错误映射：

| raft 错误 | `kv::Status` | 调用方行为 |
|-----------|--------------|------------|
| `NotLeader` | `NotLeader` | 回 ERROR 帧；P1 只报错，不代转（§11） |
| `Timeout`（读屏障 / proposal 等待） | `Timeout` | 读可重试；写必须用同一 request id 重试 |
| `InvalidArgument` / `IOError` / `InternalError` | 同名 | 透传 |

**线程规则**（P1 落地时必须遵守，否则就是数据竞争）：

- `RaftNode::tick()/handle_message()/propose()/read_barrier()` 必须在**同一个
  Raft 服务线程**上调用；
- `RaftKVEngine` 的写会阻塞等 completion，**不能在这个线程上等**；
- 正确形状是 §9 的 `RaftRuntime`：session/write 线程把 proposal
  `SubmitToService` 进去，Raft 服务线程驱动 tick / 消息 / commit / apply 并
  完成 completion；`Transport::send()` 继续只排队，不得同步重入。
- 今天 `raft_kv_store` 是"线程无关的一层皮"：它假定调用方已经串行化。
  单节点 / 单线程测试成立，接 server 前必须先有 runtime。

### 4.4 P1b：RaftRuntime、transport 与线程模型（已落地）

线程划分（谁碰什么）：

| 线程 | 碰什么 | 怎么和 RaftNode 打交道 |
|------|--------|------------------------|
| **Raft 服务线程**（`RaftRuntime`） | `RaftNode`、`StateMachine`、`LogStore` 的**唯一**访问者 | 直接调用（`tick/handle_message/propose/read_barrier`） |
| transport 接收线程（svrkit Loop） | socket、分帧缓冲 | 只 `post_message()`，**不**直接 `handle_message()` |
| transport 发送线程 | 每 peer 一条长连接 | 只读传输队列，不碰 Raft 状态 |
| SQL session / 写服务线程 | `RaftKVEngine`、`TxBuffer` | 阻塞提交 `propose()/read_barrier()`，然后等 completion |
| 定时器（`Loop::add_timer` 或测试的假时钟） | 无 | `request_tick()` |

规则与错误：

- **阻塞提交**（`run/propose/read_barrier`）把 lambda 排进服务队列，等服务线程
  跑完再返回；`propose` 返回的 `Proposal` 之后由调用方在**自己的线程**上等
  completion（§4.3 的超时语义）。服务线程从不被调用方阻塞。
- **投递**（`post_message/request_tick`）是 fire-and-forget，队列满就丢并计数
  （tick 丢一条没关系，下一个心跳周期会补）。
- 在服务线程上发起阻塞提交会**返回 `Busy` 而不是死锁**；队列满也返回 `Busy`，
  适配层映射成 `kv::Status::Busy`（可重试）。
- `stop()` 先把队列里已入队的 tick / 消息跑完再 join，保证关停期间不丢已接收
  的消息；之后 `running() == false`，再提交直接失败。

定时器：生产用 `Loop::add_timer(heartbeat_ms, ...)` 在一个只有定时器的 Loop 上
周期性地 `request_tick()`（`add_timer` 是一次性的，回调里重新挂下一次）；
测试注入假时钟 + 手动 `request_tick()`，因此选举/心跳仍然是确定性的。

**transport 拓扑**（`raft/tcp_transport.{h,cpp}`）：每个节点向每个 peer 建一条
出站连接，接收侧用 `svrkit::TcpServer`，所以一对节点之间有**两条**连接（每个
方向一条）。看起来浪费，换来的是"每条 fd 只被一条线程拥有"：

- 出站 socket 归发送线程（阻塞 connect/write），`send()` 只需要入队，永远不必
  跳到别人的事件循环上写；
- 入站 socket 归 `TcpServer` 的 Loop（非阻塞读），解帧后只做
  `RaftRuntime::post()`，不碰 `RaftNode`；
- 拨号方先发一帧**握手**（`u8 kind + u64 node id`），接收方据此知道对端是谁
  —— svrkit 不暴露对端地址，握手比拿地址更省事。

写失败时**保留未写完的字节**并在退避后重连重发：Raft RPC 本身允许重复
（AppendEntries 会重发），因此 transport 的语义是**至少一次**而不是精确一次。
出站队列满、接收侧 post 被拒 → 丢帧并计数。

**server 接线**（`server/raft_bootstrap.{h,cpp}` + `main_server.cpp`）：
本地 KVStore → `LevelDBLogStore(<log_path>/log)` →
`LevelDBRequestResultStore(<log_path>/request_results)` → `KVStateMachine` →
`RaftTcpTransport` → `RaftNode` → `RaftRuntime` → listen + 入站线程 + 心跳定时器
→ `RaftKVStore`；关闭顺序反过来（定时器 → transport → runtime → stores）。
`raft.enabled = false`（默认）时启动路径与以前完全一致。

两个值得记住的实现细节：

- **单成员组不监听**：没有 peer 连得进来，跳过 listen 让单节点 raft 也能在
  任何环境（包括禁 bind 的沙箱）跑起来；`RaftBootstrap::Options::bind_listener`
  是给测试用的显式开关。
- **`client_id` 带每进程随机盐**：幂等结果表是持久的，而状态机会跳过
  `(client_id, request_id)` 命中过的 proposal。纯自增计数在进程重启后会重复，
  于是"重启后的第一条新写"可能被当成旧请求的重放而**静默不生效**。

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
- leader 上的读必须先过 **ReadIndex**（已实现算法）：
  1. 记下请求时刻的 `commit_index` 作为**读目标**；
  2. 立刻发起一轮 heartbeat（AppendEntries），并给这一轮编号 `R`；
  3. 只有当多数派确认了**不早于本次请求发起的那一轮**
     （`peer_acked_round >= R`）时，才认为领导权仍然有效；
  4. 等 `applied_index >= 读目标` 后，读本地已 apply 状态。
- **为什么第 2/3 步不能省**：旧 leader 被分区后仍然认为自己是 leader。若复
  用历史 ack、或只等一次"刚当选时的 no-op 已提交"，分区之后它就会继续放行
  读，返回过期数据 —— 这就是 §10 里"stale leader 读"那条坑。回执只对
  "发起时间晚于读请求"的那一轮计数，所以迟到的老回执不能当证据。为了让这条
  规则可执行，AppendEntries 的**请求带轮号、响应原样回显**：leader 只把
  `response.round` 记进 `peer_acked_round`，迟到的旧轮回执自然对不上新轮号。
- **为什么不用"当前 term 的 no-op 已提交"当判据**：no-op 只在当选那一刻证明
  一次领导权；之后的分区不会产生新 no-op，等它等于永远信任一张过期的证明。
  no-op 仍然要保留，它的作用是让新 leader 能安全提交**上一个 term** 的日志。
- **代价与后续优化**：现在是"每次读、每个 key 都要一整轮 heartbeat"。把同一
  条语句 / 一个事务里的多个读合并成一次确认（read-index batching）需要时间
  上界，要么用 lease（需要时钟假设），要么注入时钟，见 §11。
- 等不到多数派 → 一整个 `election_timeout_ms` 之后返回 `Timeout`；
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
snapshot_entries    = 0                      # 日志条目数阈值，超过则 leader 生成快照并压缩（0=关闭）
# 客户端可达的 SQL 地址（id@host:port）：不是 leader 时回给客户端做重定向
sql_endpoints       = 1@127.0.0.1:5433,2@127.0.0.1:5434,3@127.0.0.1:5435
```

**已落地**（`server/config.{h,cpp}`）：上面这些键都在内置默认值表里（默认
`enabled = false`，因此不开 raft 时启动路径与以前完全一致），`ServerConfig`
提供 `raft_enabled()/raft_node_id()/raft_peers()/raft_listen{,_host,_port}()/
raft_election_timeout_ms()/raft_heartbeat_ms()/raft_log_path()`。

`validate()` 的规则：

- `heartbeat_ms < election_timeout_ms`（开着关着都检查）；
- `peers` 只要非空就解析：`<node_id>@<host>:<port>`，id ≥ 1、端口 1..65535、
  id 与 host:port 都不能重复（**关闭状态也查**，免得拼错藏到打开那天）；
- `enabled = true` 时额外要求：`peers` 非空、`node_id` 出现在 `peers` 里、
  `listen` 形如 `host:port`、`log_path` 非空。
- `sql_endpoints` 可选（同样 `id@host:port`）：id 必须是 `peers` 里的节点，
  否则直接拒绝（拼错的 id 会让重定向静默失效）。留空 = 只报 `NotLeader`，
  不给重定向目标。

静态分片（`shard.N = <start>,<end>`）还没做：P1 只有一个 group，P2 再引入
静态 range 表并**写死 `@system/*` 落 0 号组**（§2.2）。

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

| 阶段 | 内容 | 验证方式 | 状态 |
|------|------|----------|------|
| **P0** | raft 核心：选举 / 日志复制 / 提交 / apply / 快照；**in-proc transport + 可注入假时钟** | 单测：正常路径、分区、丢消息、乱序、重启、单节点→三节点。**沙箱内可全绿**（不碰 socket） | 已落地 |
| **P1a** | 单 group 适配层：`RaftKVStore` / `RaftKVEngine`、**ReadIndex 读屏障**、组内写槽、proposal 幂等 id、超时与错误映射 | `test_raft` 的 `RaftKVAdapter` 套件（单节点读写/事务/回滚/写槽、三节点复制、follower `NotLeader`、分区 leader 超时） | 已落地 |
| **P1b** | RaftRuntime 线程模型（`ServiceThread` 驱动 tick/消息/apply）、RPC 编解码、`[raft]` 配置、生产 transport（svrkit 独立端口）、server 入口接线、leader hint 回客户端 | 端到端：SQL 跑在 raft 上，杀掉 leader 后能重新选主并继续 | 已落地（单节点重启持久化、三节点真实 socket 选举/复制、NotLeader+重定向都有测试） |
| **P2** | 多 group：按表分片；`@system/*` 落 0 号组；每 group 独立 leader；**写事务跨组拒绝 + `strict`/`loose` 模式 + `CLIENT_OPTIONS` 帧**（§3.3/§3.4） | 两表并发写互不阻塞；写事务跨组报明确错误；`--cross-group-read=loose` 下 `BEGIN; SELECT a; SELECT b; COMMIT` 能跑，`strict`（默认）下报错 | 未开始 |
| **P3** | `_meta` group 管 placement、**表级 split/merge**、成员变更、follower read / lease | 需另行设计（本文档不含） | 未开始 |

**从 P0 开始**：它不碰 SQL、能确定性测试、沙箱里也能全绿，而且 P0 定下的
`LogStore` / `StateMachine` / `Transport` 三个接口决定 P1 顺不顺。

P1 拆成 a/b 两段的理由：a 段的接口和语义能在**单线程 + in-proc transport**
下全部验证完（今天已经做到）；b 段一引入线程，`RaftNode` 的"单线程调用"
前提就变成硬约束，必须和 runtime 一起设计，不能先把 adapter 接进 server。

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
11. **ReadIndex 必须用"读请求之后发起的那一轮 heartbeat"确认领导权**。
    复用历史回执、或只等"当选时的 no-op 提交"，都会让被分区的旧 leader
    继续放行读（stale read）。测试：`IsolatedLeaderCannotServeReads`。
12. **所有跨线程等待都必须有上限**。分区里的旧 leader 不会自己降级，
    `wait()` 无超时 = session 永久挂死。测试：
    `IsolatedLeaderFailsReadsAndWritesWithTimeout`。
13. **proposal 超时 ≠ 写入失败**：日志条目可能稍后提交。重试必须复用同一个
    `(client_id, request_id)`，靠状态机去重；否则一次超时重试就会重复执行。
14. **`RaftKVStore::write_batch()` 必须拒绝**（`NotSupported`）。留一条绕过
    propose 的 raw 写路径，就等于给复制留了一个静默的本地写后门。
15. **adapter 不能自己起线程，也不能在 Raft 服务线程上阻塞等待 proposal**。
    见 §4.3 的线程规则；这条在接 server 时最先被违反。
16. **transport 线程只能 `post_message()`，不能直接 `handle_message()`**。
    收到帧就内联处理 = 网络线程和 Raft 服务线程同时碰 `RaftNode`，而且会把
    慢盘/慢 apply 反压到网络线程上。
17. **`[raft] enabled = true` 在没有接线时必须报错退出**。配置解析通过不代表
    复制真的生效；静默降级成"只写本地"是最坏的一种 bug（用户以为写被复制了）。
    （接线已落地，这条现在只约束"部分接线"的未来改动：宁可启动失败。）
18. **transport 的每个方向各一条连接**：不要为了省 fd 去让接收线程在别人的
    Loop 上写 —— 那会把"每条 fd 单线程拥有"这条不变量打破，跨线程写 socket
    是最难查的一类竞态。
19. **`client_id` 必须跨重启唯一**（现在用进程随机盐）。幂等结果表是持久的，
    id 重复会让重启后的新写被当成旧请求重放而静默跳过 —— 表现为"写返回成功但
    数据没变"。
20. **启动顺序不能反**：`RaftNode::start()`（装 transport 回调）必须发生在
    inbound 线程启动之前，否则第一批消息会因为回调还没装好而丢掉。

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
- **read-index 合并 / lease**：现在每个 key 一次确认，读放大明显。合并成
  "每条语句一次"需要时间上界（lease + 时钟假设，或注入时钟），
  是 P1b 之后性能相关的第一件事。
- **结构化错误**：`NotLeader` 需要带 leader hint，`CrossGroupTransaction`
  需要带两个 group id，但现在只有 `kv::Status` 枚举。要么扩错误类型
  （`StatusInfo`），要么在 ERROR 帧里另带字段。
- **`RaftRuntime` 的细节**：已确定"一个服务线程 + 阻塞提交 + 投递"三条规则
  （§4.4）；还没定的是 apply 是否单独一条线程（现在 apply 就在服务线程上，
  `remove_range` 这类慢操作会把心跳挡住），以及 proposal 队列要不要按组拆分。
- **transport 的运维语义**：断线重连是"退避 + 重发未写字节"，没有实现连接
  保活/半开检测（对端进程僵死时只能靠写失败发现），也没有压测过 N 较大时的
  连接数（现在是 N×(N-1) 条）；要不要做多路复用留到有实测需求时再定。
- **`NotLeader` 的剩余形状**：现在 server 在语句执行前查一次 hint（阻塞提交到
  raft 服务线程，代价很小），命中就回 `NOT_LEADER` + 地址，客户端自动重连重试
  一次。还没做的是"server 代转"（把语句转发给 leader 执行再回结果）——那需要
  服务端内部再起一个客户端，等有明确收益再说。
- **schema 读路径的 NotLeader**：`Catalog` 的读接口是 `bool/optional`（
  表不存在 vs 读失败分不开）。有 hint 时 server 的预检查会先拦下，所以 follower
  上不会走到 schema 读取；但**还没学到 leader 的 follower**（刚启动 / 被分区）
  仍可能把"读不到"报成"表不存在"（或按空表处理）——彻底修需要让 Catalog 的读
  接口带三态/状态位。
- **proposal 等待上限可配置**：现在借用 `election_timeout_ms`，将来应该有
  独立的 `raft.proposal_timeout_ms`，并区分"读超时"和"写超时"。
- **affected rows / apply 结果与幂等结果的原子性**：现在 `apply_result` 固定是
  `"applied"`；一旦要做成真实 affected rows，就得和 request result 一起原子
  落盘，否则重启重放会给出不一致的返回值。
