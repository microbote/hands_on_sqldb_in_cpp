# test_raft 修改日志与实现检查

日期：2026-09-14

## 1. 当前状态

`raft/` 已从“设计文档 only”进入 **P0 core 落地**：

- 单 group `RaftNode` 已实现；
- 选举、RequestVote、AppendEntries、冲突 suffix 截断、多数派 commit、
  状态机 apply 已实现；
- 新 leader 会追加当前 term 的 no-op entry；
- in-proc transport + fake clock 已用于确定性测试；
- 生产用 `LevelDBLogStore` 已实现并接入 `sql_raft`；
- `WriteBatch + client request id` 的 proposal payload 编解码已实现；
  server / KV 适配层尚未接入。

当前测试规模：`test_raft` 15 个测试、279 个断言。全仓 `ctest` 为
14/14 通过。

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
| `tests/test_raft/test_raft.cpp` | in-proc 网络和核心测试 | 覆盖正常路径与关键错误路径 |

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

## 7. 已知未完成项

当前 P0 尚未完成：

1. **snapshot 传输 / 安装协议**：本地有限 range 生成与恢复已实现，但 Raft
   消息格式、安装期间日志暂存和重放未实现；
2. **日志 compaction**：`LevelDBLogStore` 目前要求日志从 index 1 开始；
3. **生产 transport**：尚未接 `svrkit::TcpServer`；
4. **Raft runtime 线程模型**：尚未用 `ServiceThread` 驱动 tick / message /
   apply；
5. **RaftKVStore / RaftKVEngine**：尚未接入 SQL；
6. **read-index**：leader 读等待尚未实现；
7. **幂等结果与本地 KV 的原子性**：request result 已有 LevelDB 实现，但
   目前是“本地 KV apply 成功后再保存结果”；若在两步之间崩溃，重启会重放
   同一个 `WriteBatch`。当前 `WriteBatch` 操作幂等且结果是固定 `"applied"`，
   因此安全；后续如果 apply 返回 affected rows，需要把结果持久化与状态机
   apply 做成同一个事务或恢复协议；
8. **成员变更**：设计范围外，未实现。

## 8. 后续 review 检查点

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
