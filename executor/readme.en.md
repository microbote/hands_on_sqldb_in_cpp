# executor module

中文版：[readme.md](readme.md)

## 1. Where it sits

```
SQL text --parser--> AST --StatementBuilder--> Query --Validator--> Query
         --Rewriter--> Query --Optimizer--> OptimizedQuery --Planner--> plan tree
         --ExecutorFactory--> operator tree --ResultCursor--> client
```

executor depends only on `planner` (plan tree) and `relation` (Table views +
storage cursors); it never touches the KV engine directly.

## 2. Two interfaces, do not mix them

| | `exec::Executor` (internal SPI) | `sql::Cursor` (client handle) |
|---|---|---|
| Audience | inside the execution layer | clients (CLI / drivers) |
| Methods | `open / next / close / plan` | `next / close` |
| Driven by | parent operators pulling rows | the client calling `next()` |
| Implemented by | `Scan/Filter/Sort/Limit/Project/Update/Delete/Insert Executor` | `relation::TableCursor`, `exec::ResultCursor` |

`ResultCursor` is a **facade**: it owns the root operator and forwards `next()`
— "Cursor::next() drives the executor". They are deliberately *not* related by
inheritance (we discussed it and rejected it): operators will want
vectorization/parallelism/spill while cursors want result metadata/fetch
batches, and merging them would freeze both.

## 3. Operators and plan nodes

| Plan node | Operator | Notes |
|---|---|---|
| `FullScan` / `IndexScan` / `RangeUnion` | `ScanExecutor` | range concatenation + direction + excluded keys; point lookups degrade to a single `Get` |
| `Filter` | `FilterExecutor` | `where_keeps(evaluate_condition(...))` — only TRUE passes |
| `Sort` (TopN) | `SortExecutor` | no LIMIT: full sort (over `row_limit` -> `MEMORY_LIMIT`); with LIMIT: an n-row heap |
| `Limit` | `LimitExecutor` | skip OFFSET, take LIMIT, then stop pulling from upstream |
| `Project` | `ProjectExecutor` | trim/reorder output columns per the SELECT list |
| `Update` / `Delete` | write operators | pull one row from the child, modify/delete it |
| `Insert` | `InsertExecutor` | row source is the statement's VALUES |
| DDL / USE | — | no row stream: executed by the upper layer via `sql::Catalog` (USE also needs session state) |

Implementation details:

- **Sort semantics** (`SortExecutor`): compare column by column, **NULL is
  smallest** (MySQL: NULL first for ASC); the primary key is appended as a
  tiebreaker — otherwise `LIMIT` picks an arbitrary subset of ties.
  Comparison uses `sql_order()`, falling back to the key encoding's total
  order when values are not comparable, so results are deterministic.
- **Delete** collects primary keys first and deletes in one batch: deleting
  while scanning would disturb the iterator.
- **Write statements** execute immediately in `execute()` (clients never fetch
  from them, the statement must really land); the cursor only returns `END`
  afterwards, and the affected row count comes from
  `ResultCursor::affected_rows()`.
- **SELECT opens lazily**: the root operator starts on the first `next()`, so a
  client that drops the cursor never pays for the scan.
- **End of stream** is `sql::CursorErrorCode::END` (not an error); errors are
  sticky.

## 4. Lifetime (a pitfall we hit)

Operators **copy** the semantic payload they need at run time (predicate,
order_by, LIMIT, projection columns, SET list, VALUES) into their own members
during construction, and never dereference plan nodes afterwards. Otherwise the
window between `plan` and `cursor` is a use-after-free (the first version hit
it: `FilterExecutor` read `plan_->condition()` for every row).
`Executor::plan()` is only meaningful while the plan tree is still alive
(EXPLAIN / instrumentation).

## 5. Tests

`tests/test_executor/` (44 cases):

- `test_select.cpp`: full scan / ranges / point lookups / IN range
  concatenation / excluded keys / non-PK filters / always-false conditions /
  unknown columns (UNKNOWN keeps no rows) / projection / `SELECT *` /
  cursor close / corrupt rows reported as errors.
- `test_sort_limit.cpp`: `ORDER BY pk DESC` reverse scans, **`ORDER BY pk LIMIT n`
  early stop** (a "corrupt on read" row is planted: the LIMIT query must
  succeed while the control group without LIMIT must fail), non-PK sorting,
  TopN, OFFSET, `LIMIT 0`, NULLs first, PK tiebreak, `MEMORY_LIMIT` above the
  row cap.
- `test_write.cpp`: UPDATE by PK / by non-PK / without WHERE, chained DELETE,
  INSERT (with and without a column list, missing NOT NULL, column-count
  mismatch, NULL primary key, **duplicate primary key reported as
  CONSTRAINT_VIOLATION without overwriting the old row**), no result rows and
  the affected count for writes.
- `test_text_result.cpp`: `exec::text_result()` (single-column text result set,
  used by EXPLAIN) — row count / column name / normal end / empty result /
  idempotent close.

## 6. A result set without a plan node: `text_result()`

EXPLAIN's output is not a query result, but it should not grow a separate
"print text" channel either: `exec::text_result(column, lines)` returns an
ordinary `ResultCursor` whose root operator is a `MaterializedExecutor`
(`plan() == nullptr`, it corresponds to no plan node). Clients see exactly what
they see for SELECT (`columns()` / `next()` / `close()`), so the CLI needs no
EXPLAIN branch.

## 7. Known gaps

- Multi-row `VALUES` is supported (`INSERT ... VALUES (..), (..)`; the affected
  count is the number of rows). Primary-key conflicts are reported by
  `Table::insert` as `CONSTRAINT_VIOLATION` at execution time, and the
  validator (statement layer) checks them **earlier** (duplicates inside the
  batch + conflicts with existing rows) — see `statement/README.en.md`.
- DDL / USE are not executed here (they need `Catalog` + session state).
- `SortExecutor` is in-memory only (over the cap it reports `MEMORY_LIMIT`);
  external merge sort is not implemented.
- No parallelism or chunked execution; operators are one row at a time.
  When vectorization arrives, the `ResultCursor` facade is exactly where a
  chunk -> row adapter belongs.
