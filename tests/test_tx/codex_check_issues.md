# test_tx 改动记录（KV 事务缓冲 + 引擎一致性）

这个文件记录 `tests/test_tx` 以及"事务语义"相关的改动与决策。
**上游视角**（语句级自动提交、显式 `BEGIN/COMMIT/ROLLBACK`、中止语义）
见 `tests/test_session/codex_check_issues.md` 第十一～十四节；
KV 层的设计与坑见 `storage/kv_engine/readme.md`。

## 一、当前状态（23 用例）

| 位置 | 覆盖 |
|------|------|
| `tests/test_tx/test_transaction.cpp`（15 条 Mock） | 缓冲对 DB 不可见 / 提交后可见、回滚一切照旧、读自己的写、正/反向扫描带覆盖层、`seek`/`seek_to_last` 也能看到覆盖层、单写者 `Busy`、空提交/空回滚 `NotFound`、**提交失败一条都不落且缓冲可重试**、`remove_range` 原子且顺序敏感、批量里一条非法 op 整体拒绝、缓冲大小/上限 |
| 同上（3 条 `TxLevelDb`） | 与 Mock **语义一致**：提交/回滚、读穿与扫描覆盖层、`remove_range` 删前缀 |
| 同上（5 条 `Connections`） | **多连接**：事务按连接隔离、第二条写者 `Busy` 而读者不被阻塞、扫描不被别人的提交撕裂、正/反扫描两引擎一致、连接析构 = 回滚 + 归还写槽（见第五节） |

之前踩过的坑（回归用例已钉住）：`TxIterator::normalize()` 方向用反、
LevelDB 提交持锁重入死锁（抽 `apply_batch_locked`）、`OverlayCursor` 存了
临时 `KeyRange` 的指针导致悬垂（"事务里 SELECT 只看到第一行"）、
`prev()`/`seek_to_last()` 在合并流里要取扫描方向上更靠后的候选。

## 二、本次改动

### 1. INSERT 撞主键：报错，不覆盖 —— 事务里也一样

**背景（实测）**：`Table::insert` 以前就是一次 `engine_->put`，主键已存在时
**静默覆盖**：

```
shop> INSERT INTO t (id, v) VALUES (1, 10);
OK, 1 row affected
shop> INSERT INTO t (id, v) VALUES (1, 99);   -- 以前：OK，且 v 被改成 99
```

这属于"做了奇怪的假定"（把 INSERT 悄悄当成 UPSERT），改成显式报错。

**实现**：`relation/table.cpp` 的 `Table::insert` 先做一次存在性判断，
命中直接返回错误、**不写**：

```cpp
const Key key = encode_key(pk);
if (engine_->exists(key)) {                 // 事务里走覆盖层（TxBuffer）
  return std::unexpected(RelError(
      RelErrorCode::DUPLICATE_PRIMARY_KEY,
      "duplicate primary key in " + schema_.table_name().str() + ": " +
          pk.to_string()));
}
```

**为什么"检查 + 写"没问题**（不做冲突检测的同一个理由）：写者只有一个
（悲观单写者），而且整条语句包在事务里（session 的 `AutoCommit`），
不存在"检查完被别人插队"的窗口。LevelDB 的 `WriteBatch` 本来也没有条件写，
硬要做只能靠 `get` 预读 —— 就是这里做的事，只不过主语是 `Table`。

**事务内的语义**（关键）：`exists()` 在两个引擎上都先看 `TxBuffer`：

- 同一事务里插两次同 key → **第二条立刻失败**，不用等到提交；
- 本事务删掉的行（墓碑覆盖层）→ `exists()` 返回 false → 同一个主键**允许重插**；
- 回滚之后：事务里的写全部作废，DB 里原来那行原封不动（有用例断言旧值与行数）。

**错误码链路**（每层各给自己的码，沿用各层已有的错误枚举 + 映射函数）：

| 层 | 码 | 映射位置 |
|----|----|----------|
| relation | `RelErrorCode::DUPLICATE_PRIMARY_KEY` | `relation/relation_defs.h` |
| sql_types（行流） | `CursorErrorCode::CONSTRAINT_VIOLATION` | `relation/cursor.cpp::to_cursor_error` |
| executor | `ExecErrorCode::CONSTRAINT_VIOLATION` | `executor/executor.cpp::to_exec_error` |
| session | `SessionErrorCode::CONSTRAINT_VIOLATION` | `session/session.cpp` 的 `from_exec_error` / `from_cursor_error` |

写语义上不做任何妥协：不是"后写覆盖先写"，也不是 upsert；
要 upsert 是将来 `INSERT ... ON CONFLICT` 的事。

### 2. 事务语句的执行顺序（session 侧，但直接决定事务语义）

`BEGIN/COMMIT/ROLLBACK` 现在由语法层识别（`parser/sql.y` 的
`transaction_stmt` → `NODE_TRANSACTION` → `sql::TransactionStmt`），
session 的 `execute()` 顺序是：

```
prepare() ──> 事务语句？ ──是──> begin / commit / rollback（在这里就返回）
   │否
   ├─> tx_failed_ ? ──是──> "current transaction is aborted; issue ROLLBACK"
   ├─> EXPLAIN ? ────是──> execute_explain()（中止的事务里也被这条挡住）
   └─> AutoCommit 守卫 + 计划 + 执行
```

两条顺序上的约定（都有用例）：

1. **事务语句在"已中止"检查之前**：中止状态下 `ROLLBACK` 仍然必须能跑，
   否则事务永远退不出去；
2. **EXPLAIN 在"已中止"检查之后**：中止的事务里连 `EXPLAIN` 也拒
   （`EXPLAIN ANALYZE` 会真的读数据；Postgres 也是这个语义）。

事务里的自动提交守卫**让位**：`AutoCommit` 只在引擎不在事务里时才开，
所以 `BEGIN; INSERT; COMMIT;` 里那条 INSERT 不会自己提交。

## 三、本次新增/修改的用例

| 位置 | 用例 | 覆盖 |
|------|------|------|
| `tests/test_relation/test_table.cpp` | `Table.InsertWithExistingPrimaryKeyIsRejected` | 表级：主键重复报 `DUPLICATE_PRIMARY_KEY`、**旧行原封不动**、删除后可重插 |
| 同上 | `Table.InsertInsideTransactionSeesItsOwnWrites` | 事务里插两次同 key **立刻**报错；事务内读到的还是第一次那行；回滚后 `find` 不到、重插成功 |
| `tests/test_executor/test_write.cpp` | `Write.InsertDuplicatePrimaryKeyFailsAndKeepsOldRow` | SQL 路径：失败 + 行数不变 + 换个主键立刻可用 |
| `tests/test_session/test_errors.cpp` | `SessionError.DuplicatePrimaryKeyIsConstraintViolation` | 端到端错误码 `CONSTRAINT_VIOLATION` + 旧值不变 |
| `tests/test_session/test_transaction.cpp` | `TxSession.ExplainIsRejectedInAnAbortedTransaction` | 中止的事务里 `EXPLAIN ANALYZE` 报 `TRANSACTION_ERROR` |
| `tests/test_session/test_transaction.cpp` | `TxSession.ExplainOnTransactionStatementIsRejected`（上一轮加的，与下面那条成对，一并列在这里） | `EXPLAIN BEGIN` 报 `NOT_SUPPORTED` 且**不会真的开事务** |

实测（CLI + mock 引擎；`shop>` 提示符是交互模式的样子，脚本模式不回显）：

```
shop> BEGIN WORK;
OK
shop*> INSERT INTO t (id, v) VALUES (2, 20);
OK, 1 row affected
shop*> INSERT INTO t (id, v) VALUES (2, 21);
duplicate primary key in t: 2
shop*> ROLLBACK WORK;
OK
shop> SELECT id, v FROM t;
id  v
--  --
1   10
(1 row)
```

注意最后一段：第二条 `INSERT` 报错、于是**事务被标记中止**（只能 `ROLLBACK`），
回滚后只剩事务外插进去的那一行 —— 这正是"失败语句不留半截写"的表现。
中止状态可以这样直接观察：

```
shop*> SELECT id FROM t;
current transaction is aborted; issue ROLLBACK
shop*> COMMIT;                     -- 中止状态下 COMMIT 实际执行回滚，并明确报错
transaction was aborted; rolled back instead of committing
shop> ROLLBACK;
no transaction to roll back
```

当前：sql_types 209 / parser 82 / statement 53 / planner 65 / relation 43 /
executor 43 / session 67 / **tx 18**，加 2 个 CLI 冒烟测试，`ctest` 10/10，
`--clean-first` 全量重建 0 告警；Mock 与 LevelDB 两个引擎语义一致。

## 四、还没做 / 待决定

- **写事务工作集（行锁）：暂不做**（2026-09-13 拍板，理由见第五节第 6 条）。
- **`EXPLAIN ANALYZE` 写语句**：现在直接拒绝（写语句会真改数据）。
  要放开就需要一个"在事务里跑完再回滚"的接口 —— 等价于
  "rollback-only 事务"或保存点的替代品，属于本模块的活。
- **保存点 / `COMMIT AND CHAIN` / 隔离级别**：明确不支持，语法层给具体原因
  （`ROLLBACK TO SAVEPOINT is not supported` 等）。
- **多行 `VALUES`**（`INSERT ... VALUES (a),(b)`）仍解析失败，
  所以"一行一条断言"只能一行一行测。

---

## 五、多连接：把"存储"与"连接"拆开（S1）

### 1. 为什么必须拆

在服务器上"一个进程 + N 个客户端连接"是硬需求，而原来的形状是
**一个引擎实例 = 一条连接**（`tx_` 是引擎成员）。更硬的一条理由在存储层：

> LevelDB 的目录锁挡不住**同进程**的第二次 `DB::Open`（POSIX 记录锁是按进程
> 的），两个 `leveldb::DB` 指着同一批文件写会真的写坏数据。

所以"一个进程一个 `db_` 句柄 + N 条连接"不是审美问题，是唯一正确的形状。

### 2. 改了什么

| 位置 | 变化 |
|------|------|
| `kv_engine.h` | 新增 `KVStore`（`open/close/is_open/connect/name/flush/stats/write_slot_held`）；`KVEngine` 去掉 `open_database/close_database`，改成**连接**语义（新增 `store()`、`is_open()` 转发） |
| `kv_factory.h/.cpp` | `KVEngineFactory::create` → `create_store(type)` / `open_store(type, options)`；一条连接的写法是 `store->connect()` |
| `mock_engine` | 拆成 `MockStore`（`data_` + 锁 + 写槽 + 原始读写/物化扫描）与 `MockEngine`（连接：`TxBuffer` + 覆盖层） |
| `leveldb_engine` | 拆成 `LevelDBStore`（`db_` + 锁 + 写槽 + 活跃迭代器表）与 `LevelDBEngine`（连接） |
| 写槽 | 从"引擎的 `tx_` 标志"挪到 Store：`acquire_write_slot(owner)` / `release_write_slot(owner)`；**没有显式事务的 put/remove/write_batch 也短暂占用写槽**（自动提交写），这样"单写者"在引擎级调用上也成立 |
| Mock 迭代器 | 改成**物化**：创建时在锁内把区间拷成 `vector<KVPair>`，之后不再碰 map（见下） |
| 调用点 | `cmdline` 与 tests 的 `open_engine()` 帮手改成 `open_store(...)->connect()`（含"未打开的存储"那条错误路径用例） |

连接析构时如果还带着未提交的事务：**丢弃缓冲（= 回滚）+ 归还写槽**
（不加这条，一条中途死掉的连接会把全进程的写永久锁死 —— 已进用例）。

### 3. 语义（这次定下来的）

| 项 | 选择 |
|---|---|
| 并发 | 多连接 + **悲观单写者**（写槽在 Store 上） |
| 读者 | **不阻塞、不等锁**：读已提交状态；一条扫描内部一致 |
| 第二条写者 | `begin` / 自动提交写 → `Status::Busy`（不做冲突检测，不做等待） |
| 未提交写 | 只在写者自己的 `TxBuffer` 里 → 别的连接看不到（无脏读） |
| 提交 | 一个 `WriteBatch`，原子可见（读者看到的永远是提交前或提交后） |
| 可重复读 | 写事务天然可重复（单写者，期间没有别人能提交）。**只读**可重复读事务还没做：两种实现待选（读锁持有到事务结束 / `leveldb::Snapshot`），见下 |

### 4. 扫描为什么不会被撕裂

- **LevelDB**：迭代器在创建时就钉住了当时的 seq（leveldb 自己的语义）；
- **Mock**：改成物化 —— 创建时把区间拷成 vector，之后既不读 map 也不持锁。

两条引擎因此行为一致：`Connections.ScanIsNotAffectedByAnotherConnectionsCommit`
这条用例里，A 打开扫描、B 提交新 key，A 的这条扫描看不到它，
而 A 的下一条扫描看得到。顺带好处：Mock 的迭代器**不再持有锁**，
长扫描不会把别人的提交挡住（旧实现是裸指针 + 裸迭代器，并发写 = 数据竞争）。

### 5. 本次用例（`tests/test_tx/test_connection.cpp`，5 条 × 2 引擎）

| 用例 | 断言 |
|------|------|
| `TransactionsArePerConnectionAndVisibleAfterCommit` | A 的事务写只对 A 可见（读自己的写）；B 的 get/scan 都是旧值；COMMIT 之后 B 才看到 |
| `SecondWriterIsBusyButReadersAreNotBlocked` | A 持写槽时 B 的 `begin`/`put`/`remove` → `Busy`；B 的读照旧；A 回滚后 B 可写 |
| `ScanIsNotAffectedByAnotherConnectionsCommit` | 见上（两引擎同一份断言） |
| `ReverseScanWorksOnBothEngines` | 物化迭代器的反向定位/遍历与 leveldb 一致（`c,b,a`） |
| `ClosingAConnectionRollsBackAndReleasesTheWriteSlot` | 连接析构 → 槽归还、DB 干净 |

### 6. 下一步

1. **写事务工作集（行锁）：暂不做**（拍板）。原计划是把事务读/写过的 PK key
   区间 + 点记在 Store 上、`COMMIT/ROLLBACK` 释放，对写者生效、对读者不阻塞。
   否掉的理由：
   - 现在是**单一写者**，写槽给出的互斥已经比行锁更强；行锁对读者也没有
     新语义（读者本来就看不到未提交的行），做了等于加一层没人能观察到的表；
   - 行锁真正的价值是"缩小锁范围、支持**多写者**"，但多写者 + 行锁会**幻读**：
     事务的前提可能在事务内被改掉（例如事务里先看表行数、之后按这个前提做
     判断，期间别人插删行），这个问题除非同时引入**快照读**否则很难解决。
   - 结论：行锁要和"多写者 + 快照读"作为**一件事**一起上，不能先做一半。
2. 只读事务的可重复读（可选）：读事务持共享读锁到结束（零 MVCC，
   提交要等读事务）或 `leveldb::Snapshot`（读者不阻塞提交，代价是版本管理）。
3. 语句级读一致性（读锁只在语句期间）：给将来的 join/子查询打底。
4. 引擎一致性：本次新增的 `tests/test_storage` 第一次跑就抓出三处
   "Mock 与 LevelDB 行为不一致"（remove 缺失 key、非法批量、seek 区间夹取），
   都已修好 —— 见 `tests/test_storage/codex_check_issues.md` 第二节。

---

## 六、只读事务的可重复读：`leveldb::Snapshot`

### 1. 决策

只读可重复读有两种实现：**读锁持有到事务结束**（零 MVCC，但长读事务会推迟
提交）或 **快照**（读者完全不阻塞写者）。拍板选**快照**：
`leveldb::GetSnapshot()`；Mock 用**一份数据拷贝**模拟同一语义（Mock 是测试双，
表小；这样两个引擎的可见性语义一致，不必实现多版本 + GC）。

### 2. 语义（这次定下来的）

| 时机 | 行为 |
|---|---|
| `BEGIN` | 取快照（一致读视图），**不抢写槽** —— 只读事务不挡写者 |
| 事务里的读（含元数据/schema：Catalog 读的也是这条连接） | 一律是 begin 那一刻的版本 + 自己的缓冲 |
| **第一次写** | 抢写槽：拿到 → **释放快照**；拿不到 → `Busy`，事务按"执行期错误"中止（只能 ROLLBACK） |
| `COMMIT` / `ROLLBACK` / 连接析构 | 释放快照（+ 归还写槽） |
| autocommit 语句 | 没有事务：每条语句看最新已提交状态（写语句自己包一个事务） |

**为什么"一写就释放快照"**：写槽保证没有别人能提交，所以"最新已提交 +
自己的缓冲"本身就是冻结视图 —— 可重复读在写阶段依然成立；而写路径的存在性
检查（`Table::insert` 的"主键已存在就报错"）**必须看最新状态**，否则会把
别人在本事务 begin 之后提交的同一个主键静默覆盖成重复键。也就是说：
读阶段用快照，写阶段用"写槽 + 最新状态"，两者拼起来仍然是可重复读。

### 3. 行为变化（相对上一轮）

- `BEGIN` 不再因为"别人在写"而失败：**多条连接可以同时开事务**（各拿各的
  快照）；冲突推迟到**第一条写语句**（`Status::Busy` / 会话报
  `TRANSACTION_ERROR`，消息里带 `busy`）。
- 写语句在**执行前**由 session 申请写槽（`execute()` 里），所以错误信息干净、
  而且"检查存在性 → 写入"之间没有窗口。
- leveldb 快照会钉住旧版本：`store->close()` 时若还有活跃快照返回 `Busy`
  （和活跃迭代器同一套保护）。

### 4. 用例

| 位置 | 覆盖 |
|------|------|
| `tests/test_tx/test_connection.cpp`（7 条，两引擎各跑一遍） | 第二条连接**能开事务**但一写 `Busy`、读者照常读；**快照 = 只读事务可重复读且不占写槽**（另一条连接写并提交，本事务的点读/扫描仍是旧版本，事务结束后看最新）；**一写释放快照**（能看到 begin 之后提交的新 key） |
| `tests/test_session/test_snapshot.cpp`（新，5 条） | 两个 session 共享一份存储：`BEGIN` 后 B 改并提交，A 的三次读（点查 ×2 + 扫描）都是同一版本，`COMMIT` 后看最新；只读事务期间 B 的写语句成功（**读者不阻塞写者**）；第二个写者被拒（`busy`）；**快照也覆盖元数据**（A 的事务里看不到 B 新建的表，提交后看得到）；autocommit 语句总是看最新 |

结果：`test_tx` 23 → **25 用例**，`test_session` 72 → **77 用例**，
`ctest` 11/11 通过、`--clean-first` 0 告警。
