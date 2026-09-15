# Raft

当前状态：**P0 core、P1a 适配层、P1b 运行时/传输/接线已实现**，覆盖单 group 的选举、日志复制、
commit、apply、HardState/日志持久化、`WriteBatch` proposal payload 编解码、
proposal commit/apply 等待、payload 应用到本地 KV、request id 幂等、有限
range 快照恢复、ReadIndex 读屏障，以及 `RaftKVStore`/`RaftKVEngine` 这一层
SQL 侧适配。P1b 已落地：`RaftRuntime`（单服务线程）、RPC 二进制编解码、
`[raft]` 配置段与校验、TCP transport、以及 `sqldb-server` 的启动/关闭接线
（`server/raft_bootstrap.{h,cpp}`）、**NotLeader + leader hint 的客户端重定向**
（`[raft] sql_endpoints` → ERROR 帧尾部 hint → 客户端自动重连重试一次）。
**Phase A v1（快照/压缩）已落地**：`SnapshotMetadata` + `LogStore::install_snapshot()`
（LevelDB 持久化快照点、重启恢复、压缩前缀在 `at()` 以边界项合成）、
`InstallSnapshot` RPC 编解码与 TCP 直通（**分片传输**：1 MiB/帧，绕开
64 MiB 帧上限）、leader 侧按 `[raft] snapshot_entries` 阈值生成快照并压缩
日志、**up-to-date follower 也各自本地压缩自己的日志**（不用等快照安装）、
落后/新 follower 通过快照追赶并继续复制（含真实 TCP 三/四节点端到端测试）、
`KVStateMachine` 支持无上界（整 key space）快照生成/恢复。
**仍留作后续**：read-index 合并、跨组只读模式、成员变更。

## 模块结构

| 文件 | 职责 |
| --- | --- |
| `types.h` | NodeId、LogEntry、HardState、配置和通用错误 |
| `log_store.h` | Raft 日志 / HardState 持久化接口 |
| `memory_log_store.h` | P0 测试用内存实现 |
| `leveldb_log_store.h` | 生产用 LevelDB 日志实现（独立于业务 KV 目录） |
| `proposal_payload.h` | `WriteBatch + client request id` 的二进制安全编码 |
| `kv_state_machine.h` | 解码 proposal 并把 `WriteBatch` 应用到本地 KV |
| `request_result_store.h` | client request id / apply 结果的幂等接口 |
| `leveldb_request_result_store.h` | 生产用持久化幂等结果表 |
| `clock.h` | 可注入逻辑时钟 |
| `state_machine.h` | replicated state machine 接口 |
| `transport.h` | RequestVote / AppendEntries 消息与传输接口 |
| `message_codec.{h,cpp}` | Raft RPC 二进制编解码 + 流式分帧（transport 用） |
| `peers.{h,cpp}` | 静态成员表解析（`[raft] peers`） |
| `raft_runtime.{h,cpp}` | 单服务线程：tick / 消息 / propose 串行化 |
| `tcp_transport.{h,cpp}` | 生产 transport：`TcpServer` 接收 + 每 peer 长连接发送 |
| `raft_node.{h,cpp}` | 单 group Raft 状态机 |
| `raft_kv_store.{h,cpp}` | `kv::KVStore` / `kv::KVEngine` 适配层（写走 Raft、读过 ReadIndex） |

## 契约与边界

- `RaftNode` 不开线程、不碰 socket，不依赖 SQL。
- `RaftRuntime` 是唯一允许碰 `RaftNode` 的地方：`tick/handle_message/propose/
  read_barrier` 全在它的服务线程上跑。外部线程只能用
  `propose()/read_barrier()/run()`（阻塞提交）或 `post_message()/
  request_tick()`（投递）；从服务线程自己发阻塞提交返回 `Busy`（不死锁），
  队列满同样返回 `Busy`（可重试）。
- `RaftRuntime::stop()` 先把队列里的 tick / 消息跑完再 join，所以关停期间的
  已入队工作不会静默丢失。
- `Transport` 必须把 `send()` 排队，不能同步重入 `RaftNode`。TCP transport 的
  做法：`send()` 只入队，**独立发送线程**负责连接/写；接收线程解出消息后
  `post()` 给 Raft 服务线程，绝不内联回调。
- **两个方向各一条连接**：每个节点向每个 peer 建一条出站连接（接收侧用
  `TcpServer`）。拨号方先发一帧**握手**（本节点 id）表明身份，接收方因此
  不需要对端地址。N 个节点 = N×(N-1) 条连接：规模小，换来"每条 fd 只被一条
  线程拥有"。
- 写失败时**保留未写完的字节、重连重发**：Raft RPC 允许重复（AppendEntries
  本来就可能重发），所以 transport 是**至少一次**投递，不是精确一次。
- 出站队列满、或接收侧 `post()` 被拒（raft 服务队列满）→ 丢帧并计数
  （`sent_frames()` / `dropped_frames()` / `receive_errors()` 可观测）。
- `[raft] log_path` 是两个独立 LevelDB 的**父目录**：`log/`（Raft 日志 +
  HardState）与 `request_results/`（幂等结果），都不和业务 KV 共目录。
- 单成员组不监听（没有 peer 连得进来），所以单节点 raft 可以完全离线跑。
- `client_id` 带**每进程随机盐**：幂等结果会持久化，纯自增计数在重启后会重复，
  会让新写被误判成旧请求的重放而静默跳过。
- **leader hint / 客户端重定向**：`RaftKVStore::leader_hint()` 在"本节点不是
  leader"时给出 `{node_id, 客户端 SQL 地址}`（地址来自 `[raft] sql_endpoints`，
  没配就只给 node id）；server 在执行语句前先问一次，命中直接回 `NOT_LEADER`
  + hint，不碰本地状态机；客户端只在**不在事务里**时重连并重试一次。
- 新 leader 会追加当前 term 的 no-op entry，用来安全提交前一 term 的日志。
- leader 的读要先过 `read_barrier()`（ReadIndex）：记下请求时刻的
  `commit_index`，再发一轮 heartbeat，等**多数派确认了这一轮**且本地 apply
  追上之后才算可读。复用历史 ack 或只等 no-op 提交会读到过期数据。
- `read_barrier()` / `Proposal::wait_for()` 的等待必须有上限：被分区的旧
  leader 不会自己降级，无超时等待会挂住调用方。适配层用
  `election_timeout_ms` 当预算，超时返回 `Timeout`。
- **proposal 超时 = 结果未知**：条目可能稍后才提交，重试要复用同一个
  `(client_id, request_id)`，由状态机去重。
- `RaftKVStore::write_batch()` 返回 `NotSupported`：SQL 写必须经 `connect()`
  走 propose，raw 写路径只留给状态机 apply（它直接持有本地 store）。
- `RaftKVEngine` 现在只服务**一个 group**（直接持有 `RaftNode &`）；P2 才换成
  路由 + 跨组检查。
- 事务内读自己的写靠 `TxBuffer` 覆盖层；第一次写抢**本组**写槽，抢到就释放
  本地快照（作用域从"全进程"缩到"本 group"）。
- `KVStateMachine` 支持有限 range 的快照生成与恢复；快照传输、安装期间的
  日志暂存/重放还未实现。
- `MemoryLogStore` 只用于测试；生产实现必须保证 term/vote 和日志先落盘再 ack。
- `LevelDBLogStore` 的 append / save_hard_state / truncate_suffix 都使用
  `sync=true`，满足 Raft 的持久化前 ack 约束。
- `LevelDBRequestResultStore` 的 save 使用 `sync=true`，重启后能返回同一
  client request 的原 apply 结果，并拒绝比最新 request id 更旧的请求。
- `Proposal::wait()/wait_for()` 只在 commit 且本地 apply 成功后返回；
  leader 丢失、日志 suffix 被截断或状态机 apply 失败会返回错误。
  调用方不能在驱动 RaftNode 的同一条线程上阻塞等待，否则会死锁。
- 接 server 的顺序（`server/raft_bootstrap.{h,cpp}`）：本地 KVStore →
  `LevelDBLogStore` → `LevelDBRequestResultStore` → `KVStateMachine` →
  `RaftTcpTransport` → `RaftNode` → `RaftRuntime` → listen + 心跳定时器 →
  `RaftKVStore`；关闭顺序反过来（定时器 → transport → runtime → stores）。
- Raft apply 使用 `KVStore::write_batch()` 的 raw 存储路径，绕过 session
  `TxBuffer` 和全局写槽；这是复制状态机的入口，不是客户端事务入口。
