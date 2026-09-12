# executor 模块

## 1. 位置

```
sql 文本 --parser--> AST --StatementBuilder--> Query --Validator--> Query
          --Rewriter--> Query --Optimizer--> OptimizedQuery --Planner--> Plan 树
          --ExecutorFactory--> 算子树 --ResultCursor--> 客户端
```

executor 只依赖 `planner`（计划树）与 `relation`（Table 视图 + 存储游标），
不直接碰 KV 引擎。

## 2. 两个接口，别混

| | `exec::Executor`（框架内部 SPI） | `sql::Cursor`（客户端句柄） |
|---|---|---|
| 给谁看 | 执行层内部 | 客户端（CLI / 驱动） |
| 方法 | `open / next / close / plan` | `next / close` |
| 谁驱动 | 上层算子拉它 | 客户端反复 `next()` |
| 实现者 | `Scan/Filter/Sort/Limit/Project/Update/Delete/Insert Executor` | `relation::TableCursor`、`exec::ResultCursor` |

`ResultCursor` 是**门面**：内部持有根算子，`next()` 转发给根算子 ——
"Cursor::next() 驱动执行器干活"。它们不是继承关系（曾经讨论过，否掉了）：
算子要长向量化/并行/spill，游标要长结果元信息/fetch 批量，混在一起两边都会僵。

## 3. 算子与计划节点的对应

| 计划节点 | 算子 | 说明 |
|---|---|---|
| `FullScan` / `IndexScan` / `RangeUnion` | `ScanExecutor` | 多区间拼接 + 方向 + 跳点；点查询退化成一次 `Get` |
| `Filter` | `FilterExecutor` | `where_keeps(evaluate_condition(...))`，只放行 TRUE |
| `Sort`（TopN） | `SortExecutor` | 无 LIMIT 全量排（超 `row_limit` 报 `MEMORY_LIMIT`）；有 LIMIT 用 n 行堆 |
| `Limit` | `LimitExecutor` | 跳过 OFFSET、取 LIMIT，取够就不再向上游要数据 |
| `Project` | `ProjectExecutor` | 按 SELECT 列表裁剪/排序输出列 |
| `Update` / `Delete` | 写算子 | 从 child 拉一行，改/删一行 |
| `Insert` | `InsertExecutor` | 行源是语句里的 VALUES |
| DDL / USE | — | 没有行流：由上层用 `sql::Catalog` 执行（USE 还需要会话状态） |

实现细节：

- **排序语义**（`SortExecutor`）：逐列比较，**NULL 最小**（MySQL：ASC 时 NULL 在最前）；
  末尾自动追加主键做 tiebreaker —— 否则并列时 `LIMIT` 取哪几行不确定。
  比较用 `sql_order()`，跨族不可比时退回 key 编码的全序，保证确定性。
- **删除**先收集主键再统一删：边扫边删会动到迭代器状态。
- **写语句**在 `execute()` 时立即执行（客户端不会来 fetch，语句必须真的落地），
  游标之后只会返回 `END`；受影响行数从 `ResultCursor::affected_rows()` 取。
- **SELECT 惰性 open**：第一次 `next()` 才启动根算子，客户端"拿到游标就不看了"
  不会白扫。
- **行流结束**用 `sql::CursorErrorCode::END`（不是错误），错误会粘住。

## 4. 生命周期（踩过坑）

算子在**构造时**把运行期要用的语义载荷（谓词、order_by、LIMIT、投影列、
SET 列表、VALUES）**拷成自己的成员**，运行期不再解引用计划节点。
否则 `plan -> cursor` 之间计划树一释放就是 use-after-free
（第一版就踩了：`FilterExecutor` 每行都读 `plan_->condition()`）。
`Executor::plan()` 只在计划树仍然存活时有意义（EXPLAIN/埋点）。

## 5. 测试

`tests/test_executor/`（40 用例）：

- `test_select.cpp`：全表/区间/点查/IN 区间拼接/跳点/非主键过滤/恒假条件/
  未知列（UNKNOWN 不保留行）/投影/`SELECT *`/游标 close/坏行报错。
- `test_sort_limit.cpp`：`ORDER BY pk DESC` 反向扫、**`ORDER BY pk LIMIT n` 早停**
  （在数据里埋一条"读到就报错"的坏行：加了 LIMIT 的查询必须成功，
  不加 LIMIT 的对照组必须报错）、非主键排序、TopN、OFFSET、`LIMIT 0`、
  NULL 排在最前、并列时主键 tiebreak、超出内存上限报 `MEMORY_LIMIT`。
- `test_write.cpp`：按主键/非主键/无 WHERE 的 UPDATE，链式 DELETE，
  INSERT（列清单、无列清单、NOT NULL 缺失、列数不匹配、NULL 主键）、
  写语句无结果行 + 受影响行数。

## 6. 已知缺口

- **多行 VALUES 语法未支持**：`INSERT ... VALUES (...), (...)` 目前解析失败
  （`statement` 层其实已经预留了多行处理，只差 `sql.y` 的一个产生式）。
- DDL / USE 的执行不在本模块（需要 `Catalog` + 会话状态）。
- `SortExecutor` 只有"内存 + 上限报错"，还没做外部归并排序。
- 没有并行/批量（chunk）执行；算子一次一行，后续要上向量化时
  `ResultCursor` 这层门面正好是 chunk → row 的适配点。
