# KV 层：事务（悲观单写者 + 缓冲后一次提交）

English version: [readme.en.md](readme.en.md)

## 1. 为什么是这个方案

LevelDB 给了两块积木，够用：

| 积木 | 作用 |
|---|---|
| `WriteBatch` | **原子**：写成一条 WAL 记录，恢复时整条重放，要么全在要么全不在 |
| `WriteOptions{sync=true}` | **持久**：提交前把 WAL fsync 掉 |
| `GetSnapshot()`（暂未用） | 一致读；将来做"可重复读"时挂在读事务上 |

所以事务的实现是：

```
事务期间：写 -> TxBuffer（内存），DB 不动
提交    ：TxBuffer 合成一个 WriteBatch + sync=true，一次写入
回滚    ：清空 TxBuffer
```

**"回滚后与原状态一致"是构造性成立的**——DB 从头到尾没被改过。这比
"先写再撤销（undo 日志）"简单一个数量级：不需要自己的 WAL、不需要崩溃恢复、
也不需要证明撤销是完整的。等价地说，用户设想的"应用到副本再原子切指针"，
在这套存储上就是**一次 WriteBatch**。

## 2. 组成

| 文件 | 职责 |
|---|---|
| `kv_engine.h` 里的 `KVStore` | **存储**（进程内一份）：数据 + 锁 + **写槽**；`connect()` 开一条连接 |
| `kv_engine.h` 里的 `KVEngine` | **一条连接**（= 一个 session 的存储视角）：自己的事务缓冲；多条连接共享同一个 Store |
| `tx_buffer.h/.cpp` | `TxBuffer`：有序 op log（提交顺序）+ 按 key 的覆盖视图（读用） |
| `tx_buffer.h` 里的 `OverlayCursor` | 覆盖层的**有序游标**（合并迭代器用；只遍历本事务动过的 key） |
| `merging_iterator.h/.cpp` | `MergingIterator`：DB 迭代器 + 覆盖游标归并（新插入的 key 也按序并进去） |
| `kv_engine.h` | `begin/commit/rollback_transaction`、`WriteBatch::remove_range`、`WriteBatch::sync` |
| `mock_engine` / `leveldb_engine` | 各自实现上述接口，**语义必须一致** |

### 存储 / 连接两层（多连接）

```
kv::KVStore   一份：data_（mock）/ db_（leveldb）+ 锁 + 写槽
   └─ connect() -> kv::KVEngine   一条连接 = 一个 session 的视角（自己的 TxBuffer）
        ├─ connect() 再来一条（两条连接共享同一份数据与写槽）
        └─ ...
```

用法（CLI 现在就是这么建的；搬服务器时每个客户端 `connect()` 一次）：

```cpp
auto store = kv::open_store(kv::EngineType::LEVELDB, options);  // 打开存储
auto conn  = store->connect();                                  // 一条连接
auto conn2 = store->connect();                                  // 第二条连接
```

为什么必须这样拆：LevelDB 的目录锁挡不住**同进程**的第二次 `DB::Open`
（POSIX 记录锁是按进程的），两个 `leveldb::DB` 指着同一批文件写是会写坏数据的。
所以"一个进程 = 一个 `db_` 句柄 + N 条连接"是唯一正确的形状。

**写槽在 Store 上**：`begin_transaction()` 向 Store 申请；同一时刻只允许一条
连接持有（第二条 → `Status::Busy`）。没有显式事务的 `put/remove/write_batch`
是"自动提交写"，它**短暂**占用写槽 —— 会话的写语句本来就包在事务里，
这条规则只是让引擎级调用也守同一个"单写者"约束。

**读者的可见性**（不需要快照就有的一致性）：

- 未提交的写只在写者自己的 `TxBuffer` 里 → 别的连接看不到（无脏读）；
- 提交是一个 `WriteBatch` → 读者看到的是提交前或提交后，没有中间态；
- **一条扫描内部一致**：LevelDB 迭代器在创建时就钉住了当时的版本；Mock
  迭代器在创建时把区间**物化**成 vector（不持锁、也不受后续提交影响）。
  两条引擎的行为一致，这正是 `Connections.ScanIsNotAffectedByAnotherConnectionsCommit`
  钉住的语义。

### 为什么 TxBuffer 要有两套表示

- `log_`（有序 op 列表）：提交时按原顺序重放。`remove_range` 与 `put` 的
  交错顺序会改变最终结果，所以不能只留"每个 key 最后状态"。
- `view_`（按 key 的覆盖视图）：点读要回答"这个 key 现在是什么"，它必须
  合并同一个 key 的多次写。范围删除用序号参与判定：**序号更大的操作生效**
  （`put` 之后 `remove_range` -> 删；`remove_range` 之后 `put` -> 写）。

### 覆盖层的回答是三态（别用 `optional<optional>`）

```cpp
struct OverlayOp {
  enum class Kind { kNone, kValue, kTombstone };
  Kind kind = Kind::kNone;
  ByteValue value;   // 仅 kValue 有效
};
```

- `kNone` -> 这个 key 由下层（DB）回答；
- `kValue` -> 本事务写了新值；
- `kTombstone` -> 本事务删了它（含被 `remove_range` 覆盖）。

点读和扫描（`MergingIterator`）用的是**同一个** `OverlayOp` 判定，不会出现
"扫出来的行"和"点查到的行"打架。

### 扫描为什么必须"归并"而不是"过滤"

自动提交只包"写语句自己"，那会儿过滤就够了；但显式事务里
`BEGIN; INSERT; SELECT; COMMIT` 的 SELECT 必须看到自己刚插入的行 ——
这些 key 在 DB 里**还不存在**，必须由覆盖游标按序插进结果流，所以
`MergingIterator` 做的是归并：

| 情况 | 行为 |
|---|---|
| 覆盖层有、DB 没有（新插入） | 按序插入结果流 |
| 两边都有（改过） | 用覆盖层的新值 |
| 覆盖层是墓碑（删过） | 两边一起吃掉 |
| 只有 DB 有 | 透传 |

范围删除（`remove_range`）不展开：DB 的 key 由 `lookup()` 判定被覆盖，
覆盖层自己的 key 用序号判定先后。

## 3. 语义与边界

| 项 | 我们的选择 |
|---|---|
| 并发 | **多连接 + 悲观单写者**：一个进程一份 Store，N 条连接（= N 个 session）；同时只允许一个**写**事务，第二条连接的**写**（或 `acquire_write_slot()`）返回 `Status::Busy`（`BEGIN` 本身不冲突） |
| 隔离 | 不脏读（未提交的东西不在 DB 里）；提交原子可见；单写者 => 写-写冲突不存在 |
| 读者 | **不阻塞、不等锁**：读已提交状态；一条扫描内部一致（见上） |
| 可重复读（只读事务） | **`BEGIN` 取快照**（leveldb `GetSnapshot()` / Mock 的一份数据拷贝）：事务里的读永远是 begin 那一刻的版本，而且**不占写槽** → 读者不阻塞写者 |
| 可重复读（写事务） | 拿到写槽时**释放快照**：写槽保证没有别人能提交，"最新已提交 + 自己的缓冲"本身就是冻结视图（可重复读仍成立）；而且写路径的存在性检查必须看**最新**状态，否则会把别人刚提交的同一个主键静默覆盖 |
| 冲突检测 | **不做**（单写者下不需要）。将来要多写者时按"读集 + 提交时校验"补 |
| 缺失 key 的语义 | `get`/`exists`/`remove` 在**两个引擎上一致**：`get` → `NotFound`，`remove` 一个不存在的 key → `NotFound`（leveldb 的 `Delete` 原生返回 OK，`LevelDBStore::remove` 因此先查一遍）。**事务里**的删除是幂等的（进缓冲的 op），非事务的删除是"即时"语义 |
| 提交失败 | 缓冲**保留**，调用方可以重试或回滚（`LevelDB/mock` 行为一致） |
| 事务上限 | `TxBuffer::kDefaultLimit` = 64MB，超了 `commit` 返回 `InvalidArgument` |
| 显式事务 | `BEGIN / COMMIT / ROLLBACK`（含 `START TRANSACTION` / `END` / `ABORT` 别名）：**语法层关键字**（`parser/sql.y` 的 `transaction_stmt`），执行与会话状态在 session（自动提交守卫在显式事务里让位）。`BEGIN` 只取快照、不抢写槽；**第一条写语句**才抢（抢不到 → `Busy`，按"执行期错误"中止事务，只能 `ROLLBACK`） |
| 快照的资源代价 | leveldb 的 Snapshot **钉住旧版本**（阻止 compaction 回收）→ 长事务 = 空间放大；因此快照在 `COMMIT/ROLLBACK`、连接析构、`store->close()` 全路径释放（还有活跃快照时 `close()` 返回 `Busy`）。Mock 是数据拷贝，没有这层代价 |
| 事务里语句失败 | 事务标记为中止：后续语句报错，只能 `ROLLBACK`；此时 `COMMIT` 实际执行回滚并明确报错（Postgres 风格） |
| 校验期错误 | 因为还没写任何东西，**不中止**事务（MySQL 风格） |

## 4. 与上层的关系

- **session**：每条写语句/DDL 用一个 `AutoCommit` 守卫包起来
  （构造时 begin、成功时 commit、提前 return 时析构自动 rollback）
  => **语句原子性**：中途失败不会留下半截写，DDL 也不会留下
  "schema 有、名单没有"的半成品。
  显式事务里守卫让位（不新开、不提交），由 `COMMIT/ROLLBACK` 收尾；
  **读语句永远不开事务**。
- **relation**：`Table::truncate` 与 `KVCatalog::remove_prefix` 改用
  `WriteBatch::remove_range`（事务里只占一条 op，不用先收集所有 key）。
- 读语句（SELECT）**不开事务**：不需要原子性，也不该长期占着写锁。

## 5. 测试

`tests/test_storage`（19 条，存储层：两引擎同一份断言）+
`tests/test_tx`（25 条）：

- 缓冲写对 DB 不可见（`size()` 绕过缓冲直接看底层）、提交后可见；
- 回滚 = 一切照旧，且之后能正常开新事务；
- 事务内读自己的写（put -> remove -> put 的覆盖顺序）；
- 正/反向扫描都应用覆盖视图；
- 同一时刻只有一个写事务（`Busy`）；空提交/空回滚返回 `NotFound`；
- **提交失败**（故障注入）：一条都不落 + 缓冲保留可重试；
- `remove_range` 的顺序敏感性（put 后删 vs 删后 put）；
- 一批里有一条非法 op 时**整体拒绝**（与 LevelDB 的 WriteBatch 原子性对齐）。
- `TxLevelDb` 三条：**同一套语义在 LevelDB 引擎上再跑一遍**
  （这条是血泪教训：LevelDB 侧曾在 commit 里持锁重入导致死锁，当时只有
  Mock 引擎进了测试）。
- 合并迭代器：新插入的 key 出现在正/反向扫描里、覆盖已有 key、删掉的
  key 消失、`seek` / `seek_to_last` / `prev` 也能看到覆盖层里的 key。
- **`Connections` 七条（同一份用例在 Mock 与 LevelDB 上各跑一遍）**：
  连接各自的事务缓冲互不可见、提交后互相可见；第二条连接**可以开事务**
  （拿快照）但一写就 `Busy`，而读者照常读；扫描期间别的连接提交
  **不影响这条扫描**；正/反扫描在两个引擎上一致；连接带着未提交事务析构
  = 回滚 + 归还写槽；**快照 = 只读事务的可重复读且不占写槽**；
  **一写就释放快照**（存在性检查看最新）。

## 6. 实现过程中踩到的坑（都进了测试）

1. `TxIterator::normalize()` 方向用错：`Iterator::next()` 本身就是"按扫描方向前进"，
   反向迭代器内部是 `--`，我却按 direction 去调 `prev()` —— 反向扫描一行都扫不到。
2. LevelDB 侧提交死锁：`commit_transaction()` 持锁又调 `write_batch()`（同锁重入）。
   现在两个引擎都抽了 `apply_batch_locked()`（"调用方必须已持锁"）。
3. **`OverlayCursor` 存了扫描区间的指针**：调用方传进来的往往是临时 `KeyRange`
   （比如 `Table::scan` 里的 `kv_range`），表扫到第一行之后指针就悬垂了 ——
   表现是"事务内 SELECT 只看到第一行"。边界现在**复制**到游标里。
4. 合并流的 `prev()`/`seek_to_last()` 不能简单"退一格"：两个源各退一步后取
   扫描方向上**更靠后**的那个才是"前一项"。

## 7. 下一步

1. 需要多写者时再加"读集 + 提交校验"（全局序号 -> 精读集 -> 每 key 版本）；
2. **写事务工作集（行锁）：暂不做**（2026-09-13 拍板）。理由有两条：
   - 现在是**单一写者**，写槽给出的互斥已经比行锁更强，行锁对写者/读者都
     没有新的可观察语义（读者本来就看不到未提交的行）；
   - 行锁的真实用途是"缩小锁范围以支持**多写者**"，但那会引入**幻读**：
     事务的前提可能在事务内被别人改掉（例如"读到的行数"在读取前后变化，
     导致事务内的分支判断失效）。这个问题除非同时上**快照读**否则很难解决 ——
     所以行锁必须和"多写者 + 快照读"作为一件事一起做，不能先做一半。
3. ~~只读事务的可重复读~~ **已实现，选了 `leveldb::Snapshot`**（读者不阻塞
   提交）：`BEGIN` 取快照、写时释放；Mock 用一份数据拷贝模拟同一语义。
   还没做：快照导出（`SET EXPORT_SNAPSHOT`）、长事务快照超时/告警。
4. 大事务（超过 64MB）落盘 spill；这个 leveldb 缺 `DeleteRange`，整段删在
   提交时展开成逐键删（原子性不变，内存与键数成正比）。
