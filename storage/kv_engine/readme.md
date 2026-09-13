# KV 层：事务（悲观单写者 + 缓冲后一次提交）

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
| `tx_buffer.h/.cpp` | `TxBuffer`：有序 op log（提交顺序）+ 按 key 的覆盖视图（读用） |
| `tx_buffer.h` 里的 `OverlayCursor` | 覆盖层的**有序游标**（合并迭代器用；只遍历本事务动过的 key） |
| `merging_iterator.h/.cpp` | `MergingIterator`：DB 迭代器 + 覆盖游标归并（新插入的 key 也按序并进去） |
| `kv_engine.h` | `begin/commit/rollback_transaction`、`WriteBatch::remove_range`、`WriteBatch::sync` |
| `mock_engine` / `leveldb_engine` | 各自实现上述接口，**语义必须一致** |

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
| 并发 | **悲观单写者**：同时只允许一个写事务，第二个 `begin` 返回 `Status::Busy` |
| 隔离 | 不脏读（未提交的东西不在 DB 里）；提交原子可见；单写者 => 写-写冲突不存在 |
| 冲突检测 | **不做**（单写者下不需要）。将来要多写者时按"读集 + 提交时校验"补 |
| 提交失败 | 缓冲**保留**，调用方可以重试或回滚（`LevelDB/mock` 行为一致） |
| 事务上限 | `TxBuffer::kDefaultLimit` = 64MB，超了 `commit` 返回 `InvalidArgument` |
| 显式事务 | `BEGIN / COMMIT / ROLLBACK`（含 `START TRANSACTION` / `END` / `ABORT` 别名），session 层关键字 |
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

`tests/test_tx`（18 条）：

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
2. 只读事务拿 `leveldb::Snapshot` 做可重复读（接口位置已留：`Isolation`）；
3. 大事务（超过 64MB）落盘 spill；这个 leveldb 缺 `DeleteRange`，整段删在
   提交时展开成逐键删（原子性不变，内存与键数成正比）。
