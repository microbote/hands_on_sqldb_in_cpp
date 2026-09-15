# multi-raft 实现评估报告

日期：2026-09-15
评估对象：`raft/` 模块对 "multi-raft"（DESIGN §2/§3/§9 P2）的实现程度。
评估方式：读源码 + 对照 `raft/DESIGN.md` / `raft/README.md` 的 P2 验收标准。

## 结论

**尚未有效实现生产可用的 multi-raft**。当前状态是：

- **多组语义在适配层实现并有测试**（路由、per-group 写槽、跨组事务规则、
  loose 模式协商、结构化错误）；
- **但部署面仍是单 group**：server 启动只建一组，传输/线格式没有 group
  复用，server 没有按语句路由。

一句话：这是 "multi-raft 的语义内核"，还不是 "能跑起来的多 raft 集群"。

## 1. 已有效实现的部分（适配层，有测试）

| 能力 | 位置 | 证据 |
|---|---|---|
| 静态路由 | `raft/group_router.{h,cpp}` | `@system/*` → 0 号组；数据 range 表 `[start,end) → group_id`；`group_for / range_group / batch_group` |
| 多组适配 | `raft/raft_kv_store.{h,cpp}` | `RaftKVStore` 持有 `group_id → RaftExecutor*` + 路由表；每次读写先路由到所属组 |
| per-group 写槽 | 同上 | `write_slots_[group]`：不同组写互不阻塞，同组单写者 |
| 跨组事务规则 | `RaftKVEngine::check_group` | 首个碰数据的操作绑定组；写跨组拒绝；未写时跨组读 strict/loose、跨组后冻结只读；自动提交跨组 batch/range 拒绝 |
| loose 可配置 | `common/proto/protocol.{h,cpp}` + `client/*` | `CLIENT_OPTIONS` 帧 + 能力位协商 + CLI 开关 |
| 结构化错误 | `storage/kv_engine/kv_engine.h` + `raft/raft_kv_store.{h,cpp}` | `CrossGroupInfo{from,to}`，错误消息带 "group X -> group Y" |
| schema 读三态 | `relation/kv_catalog.{h,cpp}` | `last_read_status()`：follower 上 `open_table` 报 KV_ERROR(NotLeader) 而非 TABLE_NOT_FOUND |

测试覆盖：`tests/test_raft/test_raft_multi_group.cpp`（3 个单节点组）+ 
`tests/test_raft/test_raft_kv_store.cpp` 的 Catalog 三态用例。当前 `test_raft`
73 个测试、1718 个断言全绿；全仓 ctest 14/14 通过。

## 2. 未实现、挡住"有效 multi-raft"的硬缺口

### 2.1 server 只建一个组

`server/raft_bootstrap.cpp` 只创建一个 `RaftNode`，`group_range` 覆盖整个
key space；多组构造只在测试里出现，没接进生产启动路径。全仓库搜不到
`shard` 配置键（DESIGN §2.2 的 `shard.N = <start>,<end>` 未做）。

### 2.2 传输/线格式没有 group 字段

`raft/transport.h` 的 `Message` 只按 `NodeId` 寻址，一条物理连接无法复用承载
多个组的流量。多组生产要么每 group 独立监听端口/连接，要么给每条消息加
group id —— 两者都未实现。这是多组部署绕不开的前置。

### 2.3 server 按语句路由未接

- `server/server.cpp` 的 `execute_on_this_node` 仍是单次、group 0 的
  `leader_hint()` 预检查；
- `RaftKVStore::leader_hint()` 只报 0 号组；
- session 写语句执行前调无参 `acquire_write_slot()` **默认抢 0 号组的槽**，
  而语句真正路由到别的组时 `propose_batch` 会因槽组不匹配返回
  `CrossGroupTransaction`。即：现在把多组塞进 session 路径，跨组写会以误导性
  错误直接失败 —— 印证 server 路由是硬前提而非可选优化。

### 2.4 每 group 独立快照未做

事务仍是**全局一张** LevelDB 快照（BEGIN 对每个组各取一次 barrier 再取
快照）。DESIGN §3.3 的 "loose = N 份 per-group 快照、首次读某组才取" 需要
存储层支持，未实现。当前全局快照一致性反而更强，但会钉住整库旧版本。

### 2.5 多组 × 每组多节点从未合测

- 多组路由测试用**单节点**组（`peers = {自身}`）；
- 多节点复制只在**单组**里测过（`test_raft_tcp_cluster`）。

两者各自成立、理论可组合，但没有端到端验证（真实 socket 上的多组 × 多节点）。

## 3. 风险与注意事项

1. **路由表无重叠校验**：`GroupRouter` 允许 range 重叠且无配置校验，而所有
   组共享同一份状态机存储；一旦把同一 key 分给两个组，数据会互相踩。
   多组上线前必须加路由表校验（重叠/覆盖检测、`@system/*` 不可被数据 range
   覆盖）。
2. **`new_iterator` 跨组扫描直接拒绝**（返回 CrossGroupTransaction）：符合
   "一表一组"，但 `KeyRange::all()` 在有多组时不可用；需确认查询路径不会依赖
   全库扫描。
3. **request-results 幂等表 per-group 还是共享**：测试用每组一个
   `MemoryRequestResultStore`，生产单组是一个 `LevelDBRequestResultStore`。
   多组生产需明确（每组独立目录，或共享表按组/请求 id 复合键）。
4. **`begin_transaction` 对每个组各取一次 barrier**：组数越多 BEGIN 越贵，
   与 DESIGN 的 "N 份" 成本方向一致，需在配置文档里写明。

## 4. 距"有效 multi-raft"还差什么（按依赖顺序）

1. **传输层组复用方案**：每 group 独立监听 vs 消息里加 group id 复用连接，
   先做技术验证。
2. **server 多组启动 + 按语句路由**：bootstrap 建 N 组；`execute_on_this_node`
   按语句目标组预检查；session 写槽改为"先路由再按组获取"。
3. **路由表校验**：重叠/覆盖检测 + 配置加载。
4. **每 group 独立快照**（P2 后半，存储层改动）。

## 5. 一句话总结

语义内核合格、可测试；生产部署是单组，传输与 server 路由尚未多组化 ——
当前不能宣称"有效实现 multi-raft"，它是通往 multi-raft 的扎实地基。
