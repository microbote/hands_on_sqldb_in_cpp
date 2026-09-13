# Planner module

中文版：[readme.md](readme.md)

## 1. Pipeline

```
Client request
  |
  |  Parser            Builder                Validator                    Rewriter
SQL ----> AST tree ---------> Query built ----------> Query validated -----------> Query rewritten
  |
  |  Optimizer                    Planner                      Executor
  -----------> Query optimized ----------> Plan tree built ----------> Volcano cursor created
  |
  | Cursor returned
  ---------------> client holds the cursor -> client calls Cursor::next()
```

## 2. What this module contains

It receives a *validated Query*, rewrites it (`Rewriter`), optimizes it
(`Optimizer`) and turns it into a plan tree (`Planner`) for the executor.

- **Rewriter**: structural simplification only — condition dedup, NOT
  push-down, flattening, absorption, OR-of-EQ -> IN, IN dedup/single-element
  collapse. It returns another `Query`.
- **Optimizer**: extracts primary-key predicates, turns them into `KeyRange`s,
  normalizes/flattens/merges them and produces an `OptimizedQuery` holding
  ranges plus the remaining filter.
- **Planner**: turns the `OptimizedQuery` into a plan tree (Volcano style:
  Scan -> Filter -> Project/Sort/Limit), which the executor drives through
  `Cursor::next()`. Discrete ranges are turned into a continuous row stream by
  the *operators* (see below), not inside the scan node.

## 3. Implementation status

Code: `planner/{planner_defs.h, rewriter.{h,cpp}, optimizer.{h,cpp},
plan.{h,cpp}, planner.{h,cpp}}`. `planner/old/` is dead code and is not
compiled; its `condition.cpp` is only a reference for "NOT push-down / PK
condition extraction".

**The candidate set of `OptimizedQuery` (its single representation)**

```
candidates = (union of ranges) ∪ keys, then exclude_keys is skipped

no PK predicate (full scan) -> ranges = { KeyRange::all() }
always-false condition      -> ranges = { KeyRange::empty() }
pure point set (IN list)    -> no ranges, keys non-empty
exactly one point           -> keys = {v}, is_point_query = true
```

`is_all_scan()` and `is_empty_scan()` are mutually exclusive, so callers never
have to guess what "empty set" means. `exclude_keys` is only a **scan hint**
(the PK points of `<>` / `NOT IN`); the corresponding predicate stays in
`remaining_filter`, so results stay correct even if the executor ignores it.

**NULL policy** (consistent with `sql_types/sql_truth.h`)

| Predicate | Candidate space |
|-----------|-----------------|
| `id = 5` / `id > 5` / `id < 5` … | ranges excluding NULL (comparisons never match NULL) |
| `id IS NULL` | `null_only()`: the single NULL point |
| `id IS NOT NULL` | `non_null()`: from the first non-NULL value on |
| `id = NULL` (NULL literal on the right) | empty candidates (any comparison is UNKNOWN) |
| `id IN (1, NULL)` | keeps only `1` (a NULL element matches no row) |
| `id != 5` / `id NOT IN (…)` | full scan + `exclude_keys` hint + filter |

**Rewriter boundaries**: only *structural* simplification. All range
arithmetic (NULL / ±inf / open-closed / overflow) lives in `KeyRange` and the
optimizer — the old implementation turned `<=` into `< v+1`, which overflowed
at `INT64_MAX` and compared strings numerically. It also no longer invents
`1=1` / `1=0` sentinels: contradictory conditions are kept as-is and the
optimizer turns them into an empty `KeyRange`.

**Planner operators and chains**

Operator vocabulary: `FullScan` / `IndexScan` (one range; a point query is a
degenerate range) / `RangeUnion` (concatenated ranges) / `Filter` /
`Sort(TopN)` / `Limit` / `Project` / `Insert` / `Update` / `Delete` / DDL.

```
SELECT:  Project? -> Limit? -> Sort/TopN? -> Filter? -> RangeUnion? -> FullScan/IndexScan
UPDATE:  Update   -> Limit? -> Sort?     -> Filter? -> RangeUnion? -> FullScan/IndexScan
DELETE:  Delete   -> Limit? -> Sort?     -> Filter? -> RangeUnion? -> FullScan/IndexScan
INSERT:  Insert (empty child = row source is the statement's VALUES; INSERT ... SELECT would hang a child chain)
DDL/USE: leaf
```

Key points:

- `Filter? -> RangeUnion? -> Scan` is **one chain shared by all three
  statements** (`build_scan_chain` in `planner.cpp`): how the scan space is
  computed and filtered is expressed once; write operators only "pull one row,
  modify/delete it".
- `RangeUnion` holds an ordered array of ranges plus direction and excluded
  keys; the state machine turning "discrete ranges" into a continuous row
  stream lives in the **operator** (testable on its own), not welded into the
  scan node.
- Tree building moves data: `ranges` / `exclude_keys` / `remaining_filter` of
  the `OptimizedQuery` move into their operators, so there is one source of
  truth in the tree. Scan nodes locate tables through a lightweight `TableRef`
  (`Query` is move-only; only write/DDL nodes hold it).
- `ORDER BY` on the primary key as the first column builds no Sort — the scan
  direction is simply set to asc/desc (the PK is unique, so later sort columns
  can never break a tie). With LIMIT, Sort degrades to TopN
  (`n = OFFSET + LIMIT`).

`UPDATE`/`DELETE` without WHERE become a plain `FullScan` and must never be
treated as "zero rows to scan" (silently writing less data is the most
dangerous bug) — a dedicated test guards this.

`Planner::plan()` returns
`std::expected<std::unique_ptr<PlanNode>, PlanError>`: `PlanNode` is abstract
and returning by value would slice it.

## 4. Cost model (`planner/cost.h`) — **a placeholder**

Its purpose is to make the "statistics -> cost -> plan choice" step real in the
pipeline, not to be accurate:

- `Cost{startup, total}`: LIMIT cares about startup, everything else about total.
- **The only decision point today**: whether a sparse point set should be
  pushed down (`optimizer.cpp`). `k` point lookups ≈ `k*(seek + 1 row)`;
  full scan + filter ≈ `N*(scan a row + evaluate the predicate)`. When the point
  set is large relative to the table we fall back to a full scan — and then the
  PK predicate **must be put back into `remaining_filter`**, otherwise the plan
  scans less but returns wrong results (a cost model may never affect
  correctness).
- Constants are relative (scanning a row = 1), `seek = 10`: the point set
  degrades around `k > N/5`.
- Only one statistic exists: the table row count, read from
  `@system/tablestats` (maintained by the session after write statements; the
  `sql_types` Catalog interface needed no change because the planner takes
  statistics through a `StatsProvider` `std::function`). **Without statistics
  no decision is made** — pure rule-based behaviour.
- `EXPLAIN` prints `cost=startup..total`, `EXPLAIN ANALYZE` prints actual rows
  and elapsed time: comparing both columns is how you judge the model.
