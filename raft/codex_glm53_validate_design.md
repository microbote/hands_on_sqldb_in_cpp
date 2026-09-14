我理解的“引入 Raft 节点”是把 RaftNode 组件接进现有 sqldb 进程，而不是动态往已运行集群里加成员。后者在设计里明确不在当前范围内：成员变更、joint consensus 属于 P3，现阶
  段只能用静态 peers 配置启动固定成员集合。

  ## 核心结论

  RaftNode 不应该塞进 Session、executor，也不应该直接挂在 server::Server 里面。合理的引入点是进程级启动流程：先创建本地 KVStore，再创建 Raft 运行时，最后把本地 KVStore
  包装成 RaftKVStore 传给 SQL Server。这样 SQL 层仍然只看到 storage/kv_engine/kv_engine.h:333，符合 raft/DESIGN.md:24 的设计。

  也就是说，现有 server/main_server.cpp:97 的流程应从：

  config
    -> open local KVStore
    -> Server(store)

  变成：

  config
    -> logger
    -> open local KVStore
    -> open Raft LogStore
    -> create Transport / Clock / StateMachine
    -> create RaftNode / MultiRaft
    -> start Raft runtime
    -> RaftKVStore(local_store, MultiRaft)
    -> Server(raft_store)

  如果 raft.enabled = false，继续走现在的本地 KVStore 路径，保证默认行为完全不变。

## RaftNode 的归属和形态

  RaftNode 是“一个 raft group 内的一员”，不是“一个物理进程”。一个 sqldb 进程后面可以持有多个 RaftNode，每个对应一个 group；P1 只有一个 group，P2 才扩展成多个 group。

  RaftNode 本身应保持纯净：

  RaftNode
    <- LogStore        // term/vote/log 持久化
    <- Transport       // 消息收发
    <- Clock           // tick / election timeout
    <- StateMachine    // apply 到本地 KV

  不要在 RaftNode 内部直接 bind socket、起线程或依赖 Loop。P0 阶段先用 in-proc transport 和 fake clock 做确定性测试；P1 再接 svrkit::TcpServer 和真实定时器。这也是设计
  文档强调 P0 先行的原因。

  线程模型建议是：

  Raft transport loop
    -> post message to RaftService

  timer loop
    -> post tick to RaftService

  RaftService
    -> RaftNode 状态变更
    -> commit_index 推进
    -> 交给 ApplyService

  ApplyService
    -> StateMachine.apply()
    -> 唤醒 proposal / read-index waiter

  重点是：RaftNode 的方法只在一个调度线程上调用，内部可以不用锁；跨线程交互全部通过队列/post 完成。
  状态机 apply 不要放在协程 Loop 线程里，避免慢写或 remove_range 卡住整
  个事件循环。

  ## 启动生命周期

  P1 单 group 的启动顺序建议如下：

  1. 解析并校验 [raft] 配置。
  2. 初始化 logger。这里建议比现在更早创建 logger，因为 Raft 启动、恢复、选主日志很重要。
  3. 打开业务数据本地 LevelDB，也就是状态机数据。
  4. 打开独立 Raft LogStore，路径用 raft.log_path，不要和业务 KV 混在同一个 LevelDB。
  5. 从 LogStore 恢复 HardState、日志和 snapshot metadata。
  6. 创建 StateMachine 适配器：把 LogEntry 解码成 kv::WriteBatch，再应用到本地 KVStore。
  7. 创建 Raft transport，独立监听 raft.listen。
  8. 创建 RaftNode，静态配置为 P1 的一个 group。
  9. 启动 Raft runtime，等待本地状态追上 applied_index。
  10. 用 RaftKVStore 包装本地 store，传给 server::Server。
  11. SQL server 开始监听。即使当前节点是 follower，也可以启动，只是读写返回 NotLeader。

  关闭顺序则反向：

  stop SQL accept / drain sessions
  stop raft transport
  cancel pending proposals / read-index waiters
  stop raft apply service
  flush/persist raft state
  close raft LogStore
  close local KVStore

  不要让 RaftKVStore 析构时才隐式做这些，容易和 Server、Session、active iterator 的生命周期互相纠缠。

  ## 配置层

  设计文档里的 [raft] section 应落在现有 server::Config / ServerConfig 体系里，而不是另写一套配置解析。当前 server/config.h:72 已经有 typed facade，可以自然新增：

  bool raft_enabled() const;
  uint64_t raft_node_id() const;
  std::string raft_listen() const;
  std::vector<raft::PeerConfig> raft_peers() const;
  int64_t raft_election_timeout_ms() const;
  int64_t raft_heartbeat_ms() const;
  std::string raft_log_path() const;

  校验规则至少包括：

  - node_id 必须出现在 peers 中；
  - node_id 唯一；
  - listen 唯一；
  - 奇数个 voter 更符合 Raft 习惯，虽然技术上偶数也可运行；
  - heartbeat_ms < election_timeout_ms；
  - raft 开启时必须指定 log_path；
  - peers 中包含自己，但 transport 发消息时跳过 self；voter 计数时仍包含 self。

  P1 的 shard.0 = ,+ 可以先硬编码为“整个 key space 一个 group”。P2 再引入静态 range 表，并把 @system/* 固定路由到 group 0。

  ## SQL 侧的接法

  这是最容易被低估的部分。不能简单继承 LevelDBEngine，因为它的 commit_transaction() 会直接写本地 LevelDBStore，而 Raft 模式下 commit 必须先走 propose -> replicate ->
  commit -> apply。

  因此需要新增 RaftKVStore 和 RaftKVEngine：

  class RaftKVStore : public kv::KVStore {
    std::shared_ptr<kv::KVStore> local_;
    raft::MultiRaft *raft_;
  };

  class RaftKVEngine : public kv::KVEngine {
    std::shared_ptr<kv::KVStore> store_;
    raft::MultiRaft *raft_;
    // 自己持有 TxBuffer / transaction state
  };

  读路径：

  1. 判断目标 group 的 RaftNode 是否 leader。
  2. 不是 leader 返回 NotLeader。
  3. 是 leader 则记录当前 commit_index。
  4. 等待 applied_index >= read_index。
  5. 读本地已 apply 的数据。

  写路径：

  1. 对 WriteBatch 里所有 key 做 group 路由。
  2. 单个自动提交语句直接路由到目标 group。
  3. 显式事务在第一次写时绑定 group，并获取该 group 的写槽。
  4. commit_transaction() 把 TxBuffer::to_batch() 序列化进 Raft proposal。
  5. 等 Raft commit 和本地 apply 完成后返回 OK。
  6. 失败时保持事务可回滚；NotLeader 不应该导致本地状态被污染。

  现有 kv::Status 需要扩展：

  NotLeader
  CrossGroupTransaction

  但只有 enum 不够，因为 NotLeader 需要携带 leader hint，CrossGroupTransaction 最好携带两个 group id。可以先用错误消息携带 hint 做 P1，长期则定义结构化错误，例如
  StatusInfo { Status status; std::optional<NodeId> leader_hint; ... }，否则信息会在 Status 返回时丢掉。

  ## LogEntry 的实际 payload

  设计里 LogEntry.data 说“是序列化后的 WriteBatch”，但实际实现时不要只序列化裸 batch。建议 payload 里至少包含：

  version
  client request id
  write batch

  也就是：

  struct ProposalPayload {
    uint32_t version;
    RequestId request_id;
    kv::WriteBatch batch;
  };

  原因有两个：

  1. 幂等需要 request id，重试不能重新计算 batch。
  2. 未来加字段时可以做版本兼容，避免直接改日志格式导致旧日志不可读。

  WriteBatch 序列化必须保留语义：
  - op 顺序不能变；
  - put/remove/remove_range 的类型要完整；
  - remove_range 的 end 不能丢；
  - 不需要把 sync 标志复制进日志，它属于本地持久化策略。

  幂等结果也不要直接放进业务 KV 的可见 key space。可以放在 Raft LogStore 或单独的内部 meta 区域，否则全库 scan 或路由逻辑可能意外看到内部数据。

  ## 新节点怎么“进入”集群

  在当前设计范围内，只有一种安全的引入方式：静态配置。

  [raft]
  node_id = 1
  peers = 1@..., 2@..., 3@...

  三个节点启动时都使用同一份 peers 集合，只是各自 node_id 不同。启动晚的节点如果日志落后，会通过 AppendEntries 追日志；如果日志已被压缩，则通过 snapshot 追上。

  不能做的是：集群已经运行后，只改某个节点配置就“加一个新成员”。这会绕过 Raft 的 membership config，可能让不同节点对 quorum 有不同理解，产生分裂脑风险。真正的动态加节点
  需要成员变更协议，也就是设计里 P3 的内容。

  如果只是初始部署，建议流程是：

  1. 先为所有节点生成独立的数据目录和 raft log 目录。
  2. 三个节点使用相同 peers 列表。
  3. 可以任意顺序启动。
  4. 等多数派选出 leader。
  5. 再启动或连接 SQL 客户端。

## 建议的落地顺序

  我会按这个顺序切，风险最低：

  1. P0 core：定义 LogStore、Transport、StateMachine、Clock、RaftNode，先实现单 group 选举、日志复制、commit、apply。
  2. P0 tests：in-proc transport + fake clock，测单节点、三节点、丢消息、乱序、重启、日志截断。
  3. LogStore：用独立 LevelDB 实现 term/vote/log 持久化，所有 ack 前必须 sync。
  4. WriteBatch codec：实现带版本和 request id 的日志 payload 编解码。
  5. RaftKVStore / RaftKVEngine：先支持单 group、leader-only 读写、read-index、显式事务 commit 走 Raft。
  6. server integration：扩展 [raft] 配置，改 main_server.cpp 启动流程，默认 disabled。
  7. P2 multi-group：引入 router、per-group 写槽、@system/* 固定 group 0、跨组事务检查和 strict/loose 模式。

  一句话总结：RaftNode 应该作为进程级基础设施由 MultiRaft 持有，SQL 层只通过 RaftKVStore/RaftKVEngine 感知它；P1 先做一个 group，把选举、复制、commit、apply、read-index
  和生命周期打通，再考虑 multi-raft。