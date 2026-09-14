# Raft

当前状态：**P0 core 已实现，KV 状态机桥接已开始**，覆盖单 group 的选举、
日志复制、commit、apply、HardState/日志持久化、`WriteBatch` proposal payload
编解码、payload 应用到本地 KV、request id 幂等和有限 range 快照恢复；还没有
接入 server，也没有实现生产网络。

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
| `raft_node.{h,cpp}` | 单 group Raft 状态机 |

## P0 边界

- `RaftNode` 不开线程、不碰 socket，不依赖 SQL。
- `Transport` 必须把 `send()` 排队，不能同步重入 `RaftNode`。
- 新 leader 会追加当前 term 的 no-op entry，用来安全提交前一 term 的日志。
- `KVStateMachine` 支持有限 range 的快照生成与恢复；快照传输、安装期间的
  日志暂存/重放还未实现。
- `MemoryLogStore` 只用于测试；生产实现必须保证 term/vote 和日志先落盘再 ack。
- `LevelDBLogStore` 的 append / save_hard_state / truncate_suffix 都使用
  `sync=true`，满足 Raft 的持久化前 ack 约束。
- `LevelDBRequestResultStore` 的 save 使用 `sync=true`，重启后能返回同一
  client request 的原 apply 结果，并拒绝比最新 request id 更旧的请求。
- Raft apply 使用 `KVStore::write_batch()` 的 raw 存储路径，绕过 session
  `TxBuffer` 和全局写槽；这是复制状态机的入口，不是客户端事务入口。
