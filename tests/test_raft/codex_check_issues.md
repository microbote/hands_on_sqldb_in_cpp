# test_raft 修改日志与实现检查

日期：2026-09-14

## 1. 当前状态

`raft/` 已从“设计文档 only”进入 **P0 core + P1a 适配层 + P1b 完整链路**：

- 单 group `RaftNode` 已实现；
- 选举、RequestVote、AppendEntries、冲突 suffix 截断、多数派 commit、
  状态机 apply 已实现；
- 新 leader 会追加当前 term 的 no-op entry；
- in-proc transport + fake clock 已用于确定性测试；
- 生产用 `LevelDBLogStore` 已实现并接入 `sql_raft`；
- `WriteBatch + client request id` 的 proposal payload 编解码已实现；
  server / KV 适配层尚未接入。
- proposal 已支持 commit + apply 完成等待，以及 leadership loss /
  suffix truncation / apply failure 的错误返回。
- leader 读支持 **ReadIndex 读屏障**：用“读请求之后发起的一轮 heartbeat
  被多数派确认”证明领导权，再等本地 apply 追上；分区旧 leader 读会超时。
- `RaftKVStore` / `RaftKVEngine` 已把 SQL 侧的 `kv::KVStore`/`kv::KVEngine`
  接到单 group Raft 上：写走 propose，读过 read-index，事务在 COMMIT 时整批
  复制，raw store 写路径被拒绝。
- P1b：`RaftRuntime`（RaftNode 的唯一宿主线程）、RPC 二进制编解码 + 流式分帧、
  `[raft]` 配置段与校验、TCP transport（握手 + 双向连接 + 重连重发）、
  `server/raft_bootstrap` 与 `main_server` 接线全部落地。

当前测试规模：`test_raft` 49 个测试、927 个断言；`test_server` 51 个测试、
370 个断言（含 `[raft]` 配置 4 条、单节点 bootstrap 重启 1 条、三节点
真实 socket 1 条）。全仓 `ctest` 为 14/14 通过。

## 2. 组件与文件

| 文件 | 内容 | 验证点 |
| --- | --- | --- |
| `raft/types.h` | `NodeId`、`LogEntry`、`HardState`、`Proposal`、配置和错误 | P0 基础类型不依赖 SQL |
| `raft/log_store.h` | Raft 日志 / HardState 持久化接口 | term/vote/log 必须先落盘再 ack |
| `raft/memory_log_store.h` | 测试用内存日志 | 协议行为可脱离磁盘验证 |
| `raft/leveldb_log_store.h/.cpp` | 生产 LevelDB 日志 | 重启恢复、suffix 截断、二进制安全 |
| `raft/clock.h` | 可注入时钟 | 选举/心跳可确定性测试 |
| `raft/state_machine.h` | replicated state machine 接口 | apply/snapshot/restore 的 P0 契约 |
| `raft/transport.h` | RequestVote / AppendEntries 消息和传输接口 | transport 不同步重入 RaftNode |
| `raft/raft_node.h/.cpp` | 单 group Raft 状态机 | 选举、复制、commit、apply |
| `raft/proposal_payload.h/.cpp` | Raft 日志 payload 编解码 | 保留 WriteBatch op 顺序和二进制数据 |
| `raft/kv_state_machine.h/.cpp` | payload -> 本地 KV 状态机 | apply、幂等、快照恢复 |
| `raft/request_result_store.h` | request id / result 存储 | P0 先提供内存实现 |
| `raft/leveldb_request_result_store.h/.cpp` | 生产用持久化幂等结果表 | 重启恢复、二进制结果、旧 request 拒绝 |
| `raft/raft_kv_store.h/.cpp` | SQL 侧适配层：`RaftKVStore` + `RaftKVEngine` | 写走 Raft、读过 ReadIndex、组内写槽 |
| `raft/message_codec.h/.cpp` | Raft RPC 二进制帧 + 流式分帧 | 二进制安全、拒绝畸形帧 |
| `raft/peers.h/.cpp` | `[raft] peers` 解析 | `<id>@<host>:<port>`、去重、范围检查 |
| `raft/raft_runtime.h/.cpp` | RaftNode 的唯一宿主线程 | 提交串行化、队列背压、关停排空 |
| `raft/tcp_transport.h/.cpp` | 生产 TCP transport | 握手、双向连接、重连重发、只 post 不内联 |
| `server/raft_bootstrap.h/.cpp` | raft 启动/关闭装配（server 侧） | 顺序、单成员组不监听、错误早退 |
| `tests/test_raft/raft_test_net.h` | 测试共用的 in-proc transport / 假时钟 / 分区开关 | 两个测试文件共享同一套确定性网络 |
| `tests/test_raft/test_raft.cpp` | Raft 核心测试 | 覆盖正常路径与关键错误路径 |
| `tests/test_raft/test_raft_kv_store.cpp` | 适配层测试（`RaftKVAdapter`） | 单节点读写/事务、三节点复制、follower/分区错误 |
| `tests/test_raft/test_raft_message_codec.cpp` | 编解码测试 | 四种消息往返、畸形输入、分帧 |
| `tests/test_raft/test_raft_peers.cpp` | 成员表解析测试 | 合法/非法输入、格式化回写 |
| `tests/test_raft/test_raft_runtime.cpp` | runtime 测试 | 跨线程提交、线程亲和、背压、关停排空 |
| `tests/test_raft/test_tcp_transport.cpp` | transport 单元测试 | 握手/帧序、只 post、拒绝无握手连接、重连 |
| `tests/test_raft/test_raft_tcp_cluster.cpp` | 三节点真实 socket 集群 | 选举 + 提交 + 三个副本都 apply |
| `tests/test_server/test_raft_cluster.cpp` | bootstrap 端到端 | 单节点重启持久化；三节点真实 TCP（禁 bind 时跳过） |

## 3. 重要实现取舍

### 3.1 `LogStore` 增加 `last_index()`

设计稿原接口只有 `append/at/truncate_suffix/save_hard_state/load_hard_state`。
实现时补充了 `last_index()`，原因：

- Raft RequestVote 需要比较“最后一条日志的 index/term”；
- 如果没有 `last_index()`，只能用 `at(last + 1)` 试错；
- 试错会把“正常边界”与“存储错误”混在一起，不利于 `std::expected` 语义。

### 3.2 新 leader 自动追加 no-op entry

`become_leader()` 会追加一条当前 term 的空 entry。

原因：Raft 只能直接 commit 当前 term 的日志。如果新 leader 继承了上一
term 的未提交日志，必须先有一条当前 term 的 entry 被多数派存储，才能安全
推进 commit index，把前一 term 的 entry 一并提交。

测试中因此有几个显式预期：

- 单节点空集群选主后：`last_log_index == 1`，index 1 是 no-op；
- 三节点选主后：所有节点 commit/apply 到 no-op 的 index 1；
- 节点重启后重新选主：旧日志后追加新的 no-op，并用它提交旧日志。

这不是多余日志，而是 Raft 安全性的必要步骤。

### 3.3 election timeout 使用确定性偏移

P0 测试中的选举超时时间由：

```text
election_timeout + node_id 相关的小偏移
```

决定。这样测试不会出现三个节点每次都在同一逻辑毫秒竞选、然后反复平票的
非确定性。生产版本后续可以改为可注入随机 jitter，但 P0 优先保证测试
可复现。

### 3.4 `Transport` 必须排队发送

`RaftNode` 是线程无关的，P0 假定所有方法都由同一个调度器调用。
`Transport::send()` 不能同步调用目标节点的 `handle_message()`，否则可能
在当前节点状态更新未完成时发生重入。

测试里的 `TestTransport` 只把消息放入 `TestNetwork` 队列，由
`deliver_all()` 统一派发。

后续生产 transport 必须保持同样规则：网络线程收到消息后，先进入队列，
再由 Raft 调度线程恢复/处理。

### 3.5 follower 收到冲突 suffix 会截断

AppendEntries 处理规则：

1. `prev_log_index` 超过本地最后 index：拒绝；
2. `prev_log_term` 不匹配：拒绝；
3. 新 entry 与本地同 index、同 term 但数据不同：视为内部错误；
4. 同 index 但 term 不同：从该 index 开始 truncate suffix；
5. 追加缺失 entry。

第 3 点不应发生在正确 Raft 中：同一个 leader 的同一个 `(term,index)` 不可能
产生两份不同数据；一旦出现，说明存储或协议实现已经损坏，宁可报错。

### 3.6 proposal 完成等待

`RaftNode::propose()` 返回的 `Proposal` 带一个 `ProposalCompletion`：

- `wait()`：阻塞到完成；
- `wait_for()`：带超时等待；
- `done()`：非阻塞查询。

完成条件：

1. entry 已 commit；
2. 本地状态机 apply 成功；
3. apply result 写回 `Proposal::apply_result`。

失败条件：

1. leader 丢失：返回 `NotLeader`；
2. 新 leader 截断旧 suffix：返回 `NotLeader`；
3. 状态机 apply 失败：透传原错误；
4. 等待超时：返回 `Timeout`。

注意：`wait()` 不能在驱动 `RaftNode::tick()/handle_message()` 的同一条线程
上调用。RaftNode 的调度器必须继续推进消息和定时器；生产集成时应由
`RaftKVEngine` 所在的 write service 等待，或改成协程式等待。

## 4. LevelDB LogStore

### 4.1 key/value 布局

使用独立 LevelDB 目录，不与业务 KV 混用：

```text
hard_state -> encoded(term, voted_for)
log/<index> -> encoded(term, data)
```

`index` 使用固定 8 字节 big-endian，因此 LevelDB 的字典序就是日志 index
顺序。

### 4.2 持久化语义

以下操作都使用 `WriteOptions.sync = true`：

- `append`
- `save_hard_state`
- `truncate_suffix`

这是对设计文档“term/vote 与日志必须落盘后再 ack”的直接落实。

### 4.3 open 时做完整性检查

P0 尚无 compaction/snapshot，因此非空日志必须：

- 从 index 1 开始；
- 连续无空洞；
- entry value 至少能读出 term。

如果日志被截断在中间，`open()` 返回错误，而不是让 Raft 带着错误状态启动。

### 4.4 已验证的持久化行为

测试覆盖：

- HardState 关闭后重开仍能恢复；
- 日志 entry 重启后仍能读取；
- payload 中包含 `\0` 时不会被截断；
- `truncate_suffix` 之后重开目录，被截断 entry 仍然不存在；
- 截断后可以继续从正确 index append；
- RaftNode 使用 LevelDB 日志重启后能恢复旧日志、重新选主并 apply 旧数据。

## 5. Proposal payload 编解码

### 5.1 结构

`ProposalPayload` 包含：

```cpp
uint64_t client_id;
uint64_t request_id;
kv::WriteBatch batch;
```

对应设计中的：

- 决定 A：复制确定的 KV `WriteBatch`；
- 幂等要求：日志条目带 client request id。

### 5.2 二进制格式

格式带 magic 和 version：

```text
"SQRA"
version
client_id
request_id
op_count
op*
```

每个 op 保存：

- type：put / remove / remove_range；
- key；
- value presence flag；
- value；
- range end。

长度字段使用 `u64`，字符串按字节复制，因此 key/value 可以包含 `\0`。

### 5.3 不编码 `sync`

`WriteBatch::sync` 不进入 payload。

原因：`sync` 是本地持久化策略，属于 LogStore / 状态机 apply 的实现细节，
不是复制状态的一部分。Raft 日志本身已经由 LogStore 的 `sync=true` 保证
持久化。

### 5.4 malformed payload 必须失败

测试覆盖：

- 空 payload；
- 错误 magic；
- 不支持 version；
- 截断 payload；
- remove_range 边界非法；
- put 缺 value；
- trailing bytes。

后续 `StateMachine::apply()` 不能吞掉这些错误；解码失败必须阻止该 entry
apply，并暴露为 Raft 内部错误。

## 6. 当前测试覆盖

### 6.1 单节点

`SingleNodeElectsAndCommitsImmediately`

- 空集群单节点选主；
- no-op entry commit/apply；
- proposal 立即 commit；
- 状态机收到 proposal 数据。

### 6.2 三节点

`ThreeNodesElectReplicateCommitAndApply`

- 恰好一个 leader；
- RequestVote 获得多数；
- leader no-op 复制到 follower；
- proposal 从 leader 复制到所有节点；
  - commit index 达到 2；
  - applied index 达到 2；
  - 状态机收到 proposal。

### 6.3 错误路径

`FollowerRejectsProposal`

- follower 调用 `propose` 返回 `ErrorCode::NotLeader`。

`HigherTermStepsLeaderDown`

- 更高 term 的 AppendEntries 会把旧 leader 降为 follower；
- term / leader hint 更新；
- 降级后 proposal 返回 `NotLeader`。

`FollowerTruncatesConflictingSuffix`

- follower 本地旧 term entry 与 leader 新 term entry 冲突；
- suffix 被截断；
- 新 entry 落盘；
- commit/apply 推进。

### 6.4 LevelDB 持久化

`LevelDBLogStore.PersistsHardStateAndEntries`

`LevelDBLogStore.TruncatesSuffixDurably`

`RaftCore.LevelDBLogStoreRecoversAfterRestart`

见第 4.4 节。

### 6.5 payload codec

`ProposalPayload.RoundTripsAllWriteBatchOperations`

`ProposalPayload.RejectsMalformedData`

见第 5 节。

### 6.6 KV 状态机

`KVStateMachine.AppliesPayloadAndDeduplicatesRequestId`

- 解码 `ProposalPayload`；
- 将 `put/remove/remove_range` 原子应用到本地 KV；
- 二进制 key/value 不被截断；
- 相同 client/request id 再次 apply 返回幂等结果；
- 本地 KV 不因重复 apply 产生额外语义变化。

`KVStateMachine.RejectsMalformedEntryWithoutMutatingKV`

- malformed payload 返回 `InvalidArgument`；
- 本地 KV 不被修改。

`KVStateMachine.SnapshotsAndRestoresFiniteRange`

- 生成有限 `[start,end)` 的二进制安全快照；
- 恢复时先清空目标 range，再灌入快照；
- range 外 key 不受影响。

`KVStateMachine.SnapshotRestoreRejectsInvalidRange`

- unbounded / reverse range 拒绝恢复。

### 6.7 raw KV 接口

为了给 Raft apply 提供正确入口，`kv::KVStore` 新增：

```cpp
Status write_batch(const WriteBatch &batch);
std::unique_ptr<Iterator> new_iterator(const KeyRange &range);
```

这不是绕过安全检查，而是区分两条路径：

- 客户端事务路径：`KVEngine` + `TxBuffer` + 写槽；
- Raft committed apply 路径：`KVStore` raw batch，复制协议已经是序列化点。

Mock 与 LevelDB 都实现该接口。全仓测试通过，说明现有客户端事务路径未受影响。

### 6.8 持久化幂等结果

`LevelDBRequestResultStore.PersistsLatestResultPerClient`

- 空 client 查询返回无结果；
- 保存 request id / result 后重启仍可恢复；
- result 可以包含 `\0`；
- 每个 client 只保存最新 request id；
- 旧 request id 迟到重试会被拒绝，避免把过期操作重新 apply。

实现使用独立 LevelDB 目录，`save()` 使用 `sync=true`。

### 6.9 proposal completion

`ThreeNodesElectReplicateCommitAndApply`

- 多数派未确认前 `wait_for(1ms)` 返回 `Timeout`；
- commit + apply 后 `wait()` 返回；
- apply result 被带回。

`ProposalWaitsThroughLeadershipLoss`

- proposal 未提交时收到更高 term；
- leader 降级；
- proposal 返回 `NotLeader`。

`ProposalReportsStateMachineFailure`

- 状态机对非 no-op entry 注入 `IOError`；
- proposal 等待返回该错误；
- 错误信息不被吞掉。

## 7. ReadIndex 读屏障（P1a 新增）

### 7.1 为什么不能只等“当选时的 no-op 已提交”

最初的实现把读屏障写成“等当前任期 no-op commit 并 apply”。它能过测试，但
**不能证明读请求时刻的领导权**：no-op 只在刚当选时证明一次，之后被分区的旧
leader 不会产生新 no-op，却会一直认为自己是 leader 并继续放行读，返回过期
数据。这是典型的 stale read。

### 7.2 现在的算法

1. `read_barrier()` 只在 leader 上成功，先记下请求时刻的 `commit_index` 作为
   读目标；
2. 立刻发起一轮 heartbeat（`send_heartbeats()`），并给该轮编号 `R`
   （`heartbeat_round_` 每轮自增一次）；AppendEntries 请求带 `round` 字段，
   follower **原样回显**在响应里，重试沿用各自那一轮的号；
3. 成功回执到达时更新 `peer_acked_round_[peer] = max(..., response.round)`；
4. 当 `quorum_acknowledged(R)`（自己 + 多数 peer 的 `acked_round >= R`）且
   `last_applied_ >= 读目标` 时，才完成屏障；
5. 期间发生领导权变化（收到更高 term）→ `fail_read_barriers(NotLeader)`，
   等待方立即返回 `NotLeader`。

### 7.3 边界

- **单节点**：quorum 就是自己，屏障立即完成；若 `applied_index` 还没追上读
  目标，仍要等 apply。
- **分区 leader**：一轮 heartbeat 拿不到多数派回执，屏障不会完成 —— 适配层
  用 `election_timeout_ms` 上限把它变成 `Timeout`，而不是无限等待。
- **迟到的老回执**：读屏障要求“轮号 ≥ 请求时刻的轮号”，所以读请求之前发出的
  那一轮回执不能当证据。
- **不做 read-index 合并**：现在每个 key/每个读都走一整轮 heartbeat；合并需要
  时间界（lease 或注入时钟），留到 P1b 之后（见 `DESIGN.md` §11）。

### 7.4 测试

| 测试 | 验证点 |
| --- | --- |
| `RaftCore.ReadBarrierWaitsForQuorumConfirmation` | 未拿到多数派回执前 `wait_for` 超时；投递消息后完成，`index` 是请求时刻的 commit index |
| `RaftCore.ReadBarrierIgnoresStaleAcknowledgements` | 延后投递的旧轮回执只能满足旧屏障，不能顶替新屏障（轮号回显） |
| `RaftCore.ReadBarrierIsImmediateOnSingleNode` | 单节点不需要等回执 |
| `RaftCore.IsolatedLeaderCannotServeReads` | 分区旧 leader 拿不到多数派 → 读屏障超时（不返回过期数据） |
| `RaftCore.ReadBarrierFailsOnLeadershipLoss` | 收到更高 term → `NotLeader` |
| `RaftCore.FollowerReadBarrierReturnsNotLeader` | follower 不提供读屏障 |

## 8. KV 适配层（P1a 新增）

### 8.1 结构

| 类型 | 职责 |
| --- | --- |
| `RaftKVStore : kv::KVStore` | `connect()` 给每个 session 一条 `RaftKVEngine`；生命周期/统计代理本地 store；`write_batch()` 返回 `NotSupported`；`new_iterator()` 代理本地已 apply 状态 |
| `RaftKVEngine : kv::KVEngine` | 读：ReadIndex → 本地已 apply 状态（事务内先看 `TxBuffer`）；写：propose → 等 commit + apply；事务：`begin` 取本地快照，第一次写抢**本组**写槽，`COMMIT` 整批复制 |

P1 只有一个 group，所以引擎直接持有 `RaftNode &`；P2 换成路由。

### 8.2 关键语义

1. **raw store 写被拒绝**：`RaftKVStore::write_batch()` = `NotSupported`。
   否则会存在一条绕过复制的本地写后门；状态机 apply 走的是它自己持有的本地
   store，不经过这一层。
2. **读自己的写**：`get/exists/get_batch` 先查 `TxBuffer` 覆盖层，再查本地
   已 apply 状态。
3. **空事务不落日志**：`COMMIT` 时 `TxBuffer` 为空直接结束事务。
4. **超时语义**：读屏障和 proposal 等待都以 `election_timeout_ms` 为上限，
   分别映射到 `kv::Status::Timeout`。**proposal 超时 = 结果未知**，条目可能
   稍后提交，所以重试必须复用同一个 `(client_id, request_id)`。
5. **失败不污染本地状态**：`COMMIT` 失败时事务保持打开、可回滚；`rollback`
   只丢缓冲。
6. **写槽作用域 = 本 group**：第二次连接的写返回 `Busy`，直到第一个事务提交
   或回滚。

### 8.3 测试（`RaftKVAdapter`）

| 测试 | 验证点 |
| --- | --- |
| `LeaderEngineReplicatesAndReadsOwnWrites` | 单节点 put/get/remove 走 Raft，且本地状态机可见 |
| `ExplicitTransactionSeesOwnWritesAndCommitsAtomically` | 事务内读自己的写；COMMIT 后另一条连接才可见 |
| `RollbackDiscardsBufferedWrites` | 回滚不留痕（本地 store 也没有） |
| `TransactionHoldsTheGroupWriteSlot` | 第二个写者 `Busy`，提交后放行 |
| `FollowerEngineReturnsNotLeader` | follower 上读/写都返回 `NotLeader` |
| `MultiNodeReplicationAppliesToEveryLocalStore` | 三节点：proposal 在三个节点的本地 store 上都 apply 到同一 index |
| `IsolatedLeaderFailsReadsAndWritesWithTimeout` | 分区 leader 的读/写都超时失败，不挂死 |
| `RawStoreWritePathIsNotSupported` | raw store 写路径被显式拒绝 |

### 8.4 已知限制

1. 适配层**线程无关**：`RaftNode` 的所有方法必须在同一个 Raft 服务线程上调用；
   引擎的 `put/commit` 会阻塞等 completion，因此不能在该线程上调用。接 server
   前必须先有 `RaftRuntime`。
2. 每个读一次 read-index，读放大明显（§7.3）。
3. `apply_result` 固定为 `"applied"`，affected rows 还没做。
4. `NotLeader` 不带 leader hint（`kv::Status` 只是枚举）。

## 9. P1b 前半段：编解码 / 成员表 / runtime / 配置

### 9.1 RPC 编解码（`raft/message_codec.{h,cpp}`）

格式（整数一律大端）：

```text
frame  = u32 payload_size + payload
payload= u8 version(=1) + u8 message_type + 字段…
```

- 四种消息：RequestVoteRequest/Response、AppendEntriesRequest/Response；
- AppendEntries 的 entries 逐条编码 `index/term/data_len+bytes`，所以日志条目
  体（序列化后的 `WriteBatch`）可以是任意二进制；
- `round` 字段进线格式：读屏障靠它把回执归到正确的轮次（§7）；
- `decode_message()` 拒绝未知版本/未知类型/截断/尾随字节/非法布尔；
- `FrameDecoder` 按流式输入吐整帧：半帧返回"还没有"，长度前缀超过 64MB 或
  长度与已收字节不自洽时返回错误（调用方应断连，而不是把内存吃满）。

测试：`MessageCodec.RoundTripsEveryMessageType`（含 `\0` 二进制体）、
`RejectsMalformedPayloads`（逐一截断每种长度）、
`FrameDecoderHandlesFragmentedAndBatchedInput`、
`FrameDecoderRejectsImpossibleSizePrefix`。

### 9.2 静态成员表（`raft/peers.{h,cpp}`）

`[raft] peers = 1@127.0.0.1:5434,2@127.0.0.1:5435`：解析成 `PeerConfig{id,
host, port}`；id ≥ 1、端口 1..65535、id 与 host:port 都不得重复，允许尾随逗号
与换行。`format_peer_list()` 回写成规范形式（日志用）。

### 9.3 RaftRuntime（`raft/raft_runtime.{h,cpp}`）

| 接口 | 语义 |
| --- | --- |
| `start(queue_max)` | 起服务线程并等它就绪（线程 id 已发布，之后 `on_service_thread()` 才可信） |
| `run/propose/read_barrier` | **阻塞提交**：lambda 在服务线程跑，调用方等它结束；propose 返回的 `Proposal` 再由调用方在自己线程等 completion |
| `post_message/request_tick` | fire-and-forget：transport 线程与定时器只能走这里 |
| `stop()` | 停止收活 → **排空已入队工作** → join |

错误语义：从服务线程自己发阻塞提交 → `ErrorCode::Busy`（**不死锁**）；队列满
→ `Busy`（可重试），并计入 `rejected_submissions()`；tick 被丢单独计入
`dropped_ticks()`（丢一条 tick 没关系，下个心跳周期补）。适配层把 `Busy` 映射
成 `kv::Status::Busy`。

测试：`RaftRuntime` 套件 6 条 —— 跨线程提交并提交成功、服务线程内提交被拒、
跨线程投递消息生效、队列满不阻塞（另起 std::thread 占住服务线程）、
`stop()` 排空已入队消息、未启动时提交被拒。

### 9.4 `[raft]` 配置（`server/config.{h,cpp}`）

默认 `enabled = false`，键与校验规则见 `DESIGN.md` §7；`ServerConfig` 提供
`raft_enabled()/raft_node_id()/raft_peers()/raft_listen*()/
raft_election_timeout_ms()/raft_heartbeat_ms()/raft_log_path()`。

**故意硬失败**：`main_server.cpp` 看到 `raft.enabled = true` 直接报错退出
（P1b 的 transport/接线未完成）。理由：配置通过 ≠ 复制生效，静默跑本地存储
是这类改动里最危险的失败模式。

## 10. P1b 后半段：transport / bootstrap / 集群

### 10.1 TCP transport（`raft/tcp_transport.{h,cpp}`）

拓扑与理由：

- **每个节点向每个 peer 建一条出站连接**（发送线程独占这些 fd，阻塞
  connect/write），接收侧用 `svrkit::TcpServer`（Loop 独占这些 fd，非阻塞读）。
  一对节点两条连接，N 个节点 N×(N-1) 条 —— 换来"每条 fd 只被一条线程拥有"。
- 拨号方先发**握手帧**（`u8 kind=1 + u64 node_id`，外层还是 u32 长度前缀），
  接收方据此知道对端是谁；svrkit 不暴露对端地址，握手比拿地址更省事。
- 接收路径只做 `post()`：`handle_inbound` 解出 `Message` 后交给
  `RaftRuntime::post`，绝不内联调用 `RaftNode`。
- 写失败 → 保留未写字节 → 退避重连 → 重发。Raft RPC 允许重复，所以语义是
  **至少一次**；队列满 / post 被拒 → 丢帧并计数。
- `stop()` 走**优雅关闭**（`request_shutdown` + `shutdown(SHUT_RD)` 让读协程
  看到 EOF 后自己退出），而不是硬停 Loop —— 硬停会把挂在 `WaitFd` 上的协程帧
  连同 fd 一起悬在那里。

测试（`test_tcp_transport.cpp`，用 socketpair 注入，不需要 bind）：

| 用例 | 验证点 |
| --- | --- |
| `SendsHandshakeThenFramesInOrder` | 先握手再按序发帧；`sent_frames`/`connected_peers` 计数 |
| `ReceivesHandshakeThenPostsMessages` | 解帧只入队（回调**没有**立即触发），drain 后才拿到 (from, message) |
| `ClosesConnectionThatSkipsTheHandshake` | 第一帧不是握手 → 断连、计数，不产生消息 |
| `ReconnectsAfterWriteFailure` | 对端消失后写失败 → 自动重连（connect 被再次调用） |

### 10.2 启动装配（`server/raft_bootstrap.{h,cpp}`）

顺序：本地 KVStore（状态机存储，调用方已打开）→ `<log_path>/log`
（`LevelDBLogStore`）→ `<log_path>/request_results`
（`LevelDBRequestResultStore`）→ `KVStateMachine` → `RaftTcpTransport` →
`RaftNode` → `RaftRuntime` → listen + 入站线程 + 心跳定时器 → `RaftKVStore`；
`stop()` 反序：定时器 → transport → runtime → 两个 LevelDB。

要点：

- `RaftNode::start()` 在 raft 服务线程上执行（`runtime.run()`），且**早于**
  inbound 线程启动 —— 否则第一批消息会因为回调未装好而丢；
- 单成员组**不监听**（没有 peer 连得进来）；`Options::bind_listener` 是测试开关；
- transport 的 `post` 回调在 runtime 建好之前返回 false（那时 inbound 还没起，
  丢帧是安全的），这样绕开"transport 要 runtime、runtime 要 node、node 要
  transport"的构造环；
- `RaftKVStore::open()` 容忍本地 store 已经被打开（启动路径本来就是先开本地
  store 再包 raft）；
- 任何一步失败都返回错误字符串（`main_server` 打印后退出 1），不留半启动状态。

### 10.3 端到端测试

| 用例 | 验证点 |
| --- | --- |
| `RaftBootstrap.SingleNodeServesWritesAndSurvivesRestart` | 单节点组写成功 → 本地 store 也有 → 停止 → 用同一目录重启 → 旧数据可读，且**重启后的新写不会被幂等表误判为重放**（v1 → v2） |
| `RaftCluster.ThreeNodesReplicateOverTcp` | 三个真实节点（真实 bind/connect/心跳）选出 leader，写入提交后三个副本的本地 store 都有该 key；sandbox 禁 bind 时打印 `[skip]` 跳过 |
| `RaftTcpCluster.ThreeNodesElectAndReplicateOverRealSockets` | 用 socketpair 全互联绕开 bind：三个真实 `RaftTcpTransport` + `RaftRuntime` + `RaftNode` 在真线程上选举、提交、三个副本都 apply |

### 10.4 两个踩过的坑

1. `std::vector<char> buffer(base.string().begin(), base.string().end())`
   —— `base.string()` 调用两次产生两个不同临时对象，取到的迭代器毫无关系；
   libc++ 直接抛 `length_error("vector")`。测试里的临时目录助手都改成先存一份
   `std::string`。
2. `client_id` 原来是从 1 开始的进程内自增。幂等结果表是持久的，重启后
   client_id 会重复，于是"重启后的第一条新写"可能被状态机当成旧请求的重放而
   **静默不生效**（写返回 OK，值没变）。修法：client_id 带每进程随机盐。

## 11. 已知未完成项

尚未完成：

1. **snapshot 传输 / 安装协议**：本地有限 range 生成与恢复已实现，但 Raft
   消息格式、安装期间日志暂存和重放未实现；
2. **日志 compaction**：`LevelDBLogStore` 目前要求日志从 index 1 开始；
3. **apply 是否独立线程**：现在 apply 跑在 Raft 服务线程上，慢 apply（比如大
   `remove_range`）会把心跳/选举一起挡住 —— 需要时再拆一条 apply 线程；
4. **幂等结果与本地 KV 的原子性**：request result 已有 LevelDB 实现，但
   目前是“本地 KV apply 成功后再保存结果”；若在两步之间崩溃，重启会重放
   同一个 `WriteBatch`。当前 `WriteBatch` 操作幂等且结果是固定 `"applied"`，
   因此安全；后续如果 apply 返回 affected rows，需要把结果持久化与状态机
   apply 做成同一个事务或恢复协议；
5. **read-index 合并 / lease**：每个 key 一次确认，读放大明显；
6. **结构化错误**：leader hint（`NotLeader`）和 group id
   （`CrossGroupTransaction`）还无处携带，客户端重连策略也未定；
7. **transport 运维语义**：没有保活/半开检测（只能靠写失败发现对端僵死），
   没有压测过 N×(N-1) 条连接；要不要多路复用等实测再定；
8. **幂等结果表的增长**：每个 client_id 一行、只保留最新 request id，长期运行
   需要回收策略；
9. **成员变更**：设计范围外，未实现。

## 12. 后续 review 检查点

review P0 代码时优先检查：

1. 所有 `save_hard_state()` 成功之前不能发送 vote response；
2. 所有 `append()` 成功之前不能发送 AppendEntriesResponse；
3. commit candidate 的 entry term 必须等于当前 term；
4. follower 的 `commit_index` 不能超过本地 `last_log_index`；
5. 冲突 suffix 截断后必须重新 append；
6. 新 leader 必须追加当前 term no-op；
7. `Transport` 不能同步重入 `RaftNode`；
8. decode payload 失败必须向调用方暴露错误；
9. `LevelDBLogStore` 的写路径必须保持 `sync=true`；
10. 测试不能依赖真实时间或 socket bind。
11. snapshot restore 必须先清空精确 group range，不能清整个本地 store；
12. request result 保存失败时必须让 apply 返回错误，不能静默丢失幂等信息。
13. 不能在 Raft 调度线程上阻塞等待 Proposal；等待方必须在另一条线程；
14. proposal 只有在本地 apply 成功后才算完成，commit 本身不够。
15. ReadIndex 必须用“读请求之后发起的那一轮 heartbeat”被多数派确认；
    不能复用历史回执，也不能只等当选时的 no-op；
16. 读屏障 / proposal 等待必须有时间上限，超时映射成 `Timeout`；
17. `RaftKVStore::write_batch()` 必须保持 `NotSupported`，不能留绕过复制的
    本地写路径；
18. `RaftKVEngine` 事务内读必须走 `TxBuffer` 覆盖层（读自己的写）；
19. `RaftNode` 的所有方法只能由 `RaftRuntime` 的服务线程调用；transport 走
    `post_message`，定时器走 `request_tick`；从服务线程发起阻塞提交必须返回
    `Busy` 而不是死锁；
20. RPC 编解码必须拒绝未知版本/类型、截断、尾随字节和超限长度前缀；
21. `[raft] enabled = true` 在接线完成前必须启动即报错，不能静默跑本地存储。
22. transport 接收路径只能 `post()`，不能内联回调 `RaftNode`；每条 fd 只由
    一条线程拥有（出站归发送线程、入站归 Loop）；
23. 写失败后未发完的字节必须保留并在重连后重发（至少一次语义）；
24. `client_id` 必须跨重启唯一（幂等表是持久的，重复 id 会让新写被静默跳过）；
25. 启动顺序：`RaftNode::start()` 必须早于 inbound 线程启动。
