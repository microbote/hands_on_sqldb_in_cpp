# executor 模块实现记录（Stage 2）

范围：`executor/{exec_defs.h, executor.h, executor.cpp}`，测试 `tests/test_executor/`。

目标（按约定）：把 `plan::PlanNode` 计划树变成 Volcano 算子树，
`ResultCursor` 作为客户端门面（`Cursor::next()` 驱动执行器），
行流结束用 `CursorErrorCode::END`。

验证：`test_executor` 40 用例 / 256 断言 / 0 失败；`ctest` 6/6 通过
（sql_types / parser / statement / planner / relation / executor）；
`-Wall -Wextra` 0 告警。

---

## 一、接口定案

| 项 | 定案 | 理由 |
|----|------|------|
| 算子接口 | `Executor{open / next / close / plan}`，`next()` 返回 `std::expected<Row, CursorError>` | 一个返回通道；`END` 表示正常结束（按你的决定），错误会粘住 |
| 客户端接口 | `ResultCursor : sql::Cursor`（`next / close`） | "Cursor 属于根执行器这一层"：门面持有根算子，`next()` 转发 |
| 是否继承 | **不继承**，用组合/门面 | 算子是框架 SPI（会长向量化/并行/spill），游标是客户端 API（会长元信息/fetch 批量）；而且写语句根本没有结果行流 |
| `open/close` 的错误 | `open()` 返回 `ExecError`；`close()` 返回 `void` 且幂等 | 关闭没有有意义的失败模式，且要在析构里兜底调用 |
| `ExecError` vs `CursorError` | 行流错误统一 `sql::CursorError`（含 `END`，住在 sql_types）；`ExecError` 只管执行器生命周期/DDL/内存超限 | `sql_types` 不能反向依赖 `executor`，两个层层都要"流结束"语义 |

`exec_defs.h` 的错误码：`NOT_OPEN / ALREADY_OPEN / INVALID_ARGUMENT /
TABLE_NOT_FOUND / COLUMN_NOT_FOUND / UNSUPPORTED_PLAN / UNSUPPORTED_STMT /
SCHEMA_ERROR / MEMORY_LIMIT / IO_ERROR / INTERNAL`。

## 二、实现要点

- **ScanExecutor** 吃三种扫描节点：`FullScan`（整表）/ `IndexScan`（单区间）/
  `RangeUnion`（多区间 + 跳点）。降序 = 区间数组**逆序** + 每个区间反向扫；
  点区间退化成一次 `Table::find()`（`IS NULL` 的 NULL 点除外，走扫描）；
  `exclude_keys` 归并跳过（对应谓词仍在 Filter 里兜底）。
- **FilterExecutor** 用 `sql::evaluate_condition` + `where_keeps`（三值逻辑），
  列值通过 schema 的列下标查；未知列 → 谓词为 UNKNOWN → 不保留该行。
- **SortExecutor** 同上节的排序语义；TopN 用大小为 `offset+limit` 的堆，
  单遍扫描；全量排序超过 `row_limit`（默认 100 万行）报 `MEMORY_LIMIT`。
- **LimitExecutor** 取够 LIMIT 就不再向上游要数据 —— 这是 `ORDER BY 主键 + LIMIT`
  早停的落点。
- **写算子** 在 `open()` 里把活干完：UPDATE 拉一行改一行；DELETE 先收主键再统一删；
  INSERT 按列清单/VALUES 顺序拼行（缺列置 NULL，`validate_row` 会把
  NOT NULL 缺失拒掉）。`next()` 直接 END，`affected_rows()` 给调用方。
- **ExecutorFactory** 沿 `child()` 递归建树；校验计划里的表名与传入的
  `sql::Table` 一致（防止把 A 表的计划套到 B 表上）；DDL/USE 返回
  `UNSUPPORTED_STMT`。
- **execute()**：写语句立即执行（`open_now()`），行流语句惰性 open。

## 三、写代码过程中抓到的三个真 bug

### 1. 算子运行期解引用计划节点 → use-after-free（本模块自身的 bug）

第一版让算子保存 `const XxxPlan* plan_` 并在运行期读它（`FilterExecutor` 每行
读 `plan_->condition()`、`SortExecutor::open()` 读 `plan_->order_by()`、
`ProjectExecutor::open()` 读投影列、`LimitExecutor` 每行读 LIMIT）。
计划树是 planner 的一次性产物，`plan -> cursor` 之间释放后就悬空了 ——
测试表现为"有没有 Filter 节点"决定崩不崩（`SELECT *` 不崩、`WHERE` 崩）。

修法：算子在**构造时把语义载荷拷成自己的成员**（谓词 clone、order_by、
LimitClause、投影列、SET 列表、VALUES），运行期不再解引用计划节点；
`plan()` 保留但只在计划树存活时有意义（EXPLAIN/埋点）。

### 2. `LIMIT 0` 被当成"没有 LIMIT"（parser/statement 层的 bug）

`LimitNode` 只有 `limit/offset` 两个 int，`build_limit()` 用 `if (limit > 0)`
判断是否设置 —— 于是 `SELECT ... LIMIT 0` 等价于没有 LIMIT，返回全表，
而 SQL 语义是"返回零行"。

修法：`LimitNode` 增加 `has_limit/has_offset` 标志位，`sql.y` 三个产生式
（`LIMIT n`、`LIMIT n OFFSET m`、`LIMIT offset, count`）显式传标志，
`build_limit()` 改看标志位。`print_limit_node` 对没写的部分打印 `-`，
这样 `LIMIT 0` 与"没写 LIMIT"在调试输出里也能区分。
`tests/test_parser/test_ast_nodes.cpp` 的三处直接构造调用同步更新。

### 3. `ASan` 之外的坑：`unite` 之后区间顺序（已在上一步修）

（见 planner 记录第七节：`build_scan_chain` 与 `std::move(query)` 的实参求值顺序。）

## 四、测试覆盖（40 用例）

| 文件 | 用例 | 覆盖 |
|------|------|------|
| `test_select.cpp` | 14 | 全表/区间/点查、IN 乱序输入→主键升序输出、`!=`/`NOT IN` 跳点、非主键过滤、主键+过滤组合、恒假条件零行、未知列 UNKNOWN、投影列顺序、`SELECT *`、`close()` 幂等、坏行报 `SCHEMA_ERROR` |
| `test_sort_limit.cpp` | 10 | `ORDER BY pk DESC` 反向扫、**`ORDER BY pk LIMIT 3` 早停**（数据里埋一条"读到就报错"的坏行：加 LIMIT 的查询必须成功、不加 LIMIT 的对照组必须报错）、非主键排序、TopN、OFFSET、`LIMIT 0`、NULL 排最前、并列按主键 tiebreak、超内存上限报 `MEMORY_LIMIT` |
| `test_write.cpp` | 16 | 主键/非主键/无 WHERE 的 UPDATE、NOT NULL 写 NULL 失败、区间/IN/无 WHERE 的 DELETE、INSERT（列清单、无列清单、多行列数不匹配、NOT NULL 缺失、NULL 主键）、写语句无结果行 + 受影响行数、INSERT→SELECT 往返 |

`test_executor/test_util` 里的 `run_sql()` 走完整链路（parser → builder →
optimizer → planner → table → executor），保证测的是真实计划而不是手搓的算子。

## 五、已知缺口

- **多行 VALUES 未支持**：`INSERT ... VALUES (...), (...)` 解析失败
  （statement 层已预留多行处理，缺 `sql.y` 的一个产生式）。
  测试 `Write.InsertMultiRowValuesIsNotSupportedYet` 把这个缺口钉住了，
  等 parser 放开后改成断言 `affected_rows == 2`。
- **DDL / USE 的执行不在 executor**：它们没有行流，由上层用 `sql::Catalog`
  执行（USE 还需要会话状态，不在 Catalog 接口里）。
- **外部排序没做**：全量排序只有"内存 + 上限报错"。
- **没有针对 `RangeUnion` 的独立单测**：目前通过 `IN (...)` 端到端覆盖
  （正序、乱序输入、跳点），后续可以单独构造多区间 + 降序组合。
