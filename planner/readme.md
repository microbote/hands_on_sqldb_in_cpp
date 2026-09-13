## Planner模块

### 1. 流程

```
Client Request
|
|     Parser            Builder               Validator                    Rewriter
sql -------> AST tree --------> Query Built ----------> Query Validated -----------> Query Rewrited -
----------------------------------------------------------------------------------------------------|
|
| Optimizer                     Planner				        Executor
------------> Query Optimized ----------> Plan Tree Built -----------> Valcano Model Cursor Created -
----------------------------------------------------------------------------------------------------|
|
| Cursor return
---------------> Client got the Cursor -> Client call Cursor::next() 
```



### 2. Planner模块组成

本模块接收"`Query Validated`", 经过 Rewriter 重写，`Optimizer`优化器进行优化后，由Planner规划产生 Plan树，交给下个模块Executor来执行。

- Rewriter:  接受Query，主要进行条件去重、常量折叠、not谓词下推、扁平化，再简化条件表达式，还是生成Query对象。
- Optimizer:  先进行主键抽取，将条件范围转化为区间KeyRange， 并且进行规范化扁平化，然后合并区间，生成包含KeyRange and 剩余过滤条件的OptimizedQuery对象。这里我不清楚该如何设计代价模型，暂时不做，只是加入凭经验的规则。
- Planner: 接受OptimizedQuery对象，将其转化成plan树，即volcano模型：Scan -> filter -> Projection， 要支持Cursor::next()调用。注意Optimizer产生的区间可能是离散的集合，但是这一层要维持状态，提供连续的假象。Plan对象是数值对象，可能具体如何执行和提供连续的next()的访问，需要放在执行层去考虑。这一层只是优化ScanPlan。

### 3. 实现状态（本轮）

代码：`planner/{planner_defs.h, rewriter.{h,cpp}, optimizer.{h,cpp}, plan.{h,cpp}, planner.{h,cpp}}`。
`planner/old/` 是废弃代码，不参与编译；其中的 `condition.cpp` 只作为
"NOT 下推 / 主键条件抽取" 的思路参考。

**OptimizedQuery 的候选集合（唯一表示）**

```
候选 = (⋃ ranges) ∪ keys      再跳过 exclude_keys

无主键条件（全表）  -> ranges = { KeyRange::all() }
条件恒假（矛盾）    -> ranges = { KeyRange::empty() }
纯点集（IN 列表）   -> ranges 空、keys 非空
恰好一个点          -> keys = {v}、is_point_query = true
```

`is_all_scan()`（全表）与 `is_empty_scan()`（扫不到行）互斥，调用方不必
再猜"空集合"代表什么。`exclude_keys` 只是**扫描提示**（`<>` / `NOT IN` 的
主键点），对应的谓词同时保留在 `remaining_filter` 里，因此即使执行器忽略
这个提示，结果依然正确。

**NULL 策略**（与 `sql_types/sql_truth.h` 的三值逻辑一致）

| 谓词 | 候选空间 |
|------|----------|
| `id = 5` / `id > 5` / `id < 5` … | 不含 NULL 的区间（比较不匹配 NULL） |
| `id IS NULL` | `null_only()`：只有 NULL 的单点 |
| `id IS NOT NULL` | `non_null()`：从第一个非 NULL 值开始 |
| `id = NULL`（右侧是 NULL 字面量） | 空候选（任何比较都是 UNKNOWN） |
| `id IN (1, NULL)` | 只收 `1`（NULL 元素不命中任何行） |
| `id != 5` / `id NOT IN (…)` | 全表 + `exclude_keys` 提示 + 过滤 |

**Rewriter 的边界**：只做**结构**化简（NOT 下推、扁平化、去重、吸收律、
OR-of-EQ → IN、IN 去重/单元素退化）。区间算术（NULL / ±∞ / 开闭 / 溢出）
全部交给 `KeyRange` 与 Optimizer 一处完成 —— 旧实现里
`<=` 转 `< v+1` 会在 `INT64_MAX` 溢出，也把字符串比较当成了数值比较。
另外不再造 `1=1` / `1=0` 这类常量哨兵：矛盾条件原样保留，由 Optimizer
转成空 `KeyRange`。

**Planner 的算子与链**

算子词汇表：`FullScan` / `IndexScan`（单个区间，点查询就是退化区间）/
`RangeUnion`（多区间拼接）/ `Filter` / `Sort(TopN)` / `Limit` / `Project`
/ `Insert` / `Update` / `Delete` / DDL。

```
SELECT:  Project? -> Limit? -> Sort/TopN? -> Filter? -> RangeUnion? -> FullScan/IndexScan
UPDATE:  Update   -> Limit? -> Sort?     -> Filter? -> RangeUnion? -> FullScan/IndexScan
DELETE:  Delete   -> Limit? -> Sort?     -> Filter? -> RangeUnion? -> FullScan/IndexScan
INSERT:  Insert（child 为空 = 行源是语句里的 VALUES；将来 INSERT ... SELECT 挂子链）
DDL/USE: 叶子
```

关键点：

- `Filter? -> RangeUnion? -> Scan` 这段是**三个语句共用的一条链**
  （`planner.cpp` 的 `build_scan_chain`）：扫描空间怎么算、怎么过滤只表达一次，
  写算子只做"拉一行 → 改/删一行"。
- `RangeUnion` 持有有序区间数组 + 方向 + 跳点（`exclude_keys`），
  "离散区间 → 连续行流"的状态机住在**算子**里（可单独测试），
  而不是焊在扫描节点内部。
- 建树时"搬家"：`OptimizedQuery` 的 `ranges`/`exclude_keys`/`remaining_filter`
  被移进对应算子，树里只有一份真相；扫描节点用轻量的 `TableRef`
  定位表（`Query` 是 move-only，整棵树里只有写语句/DDL 节点持有它）。
- `ORDER BY` 首列是主键 → 不建 Sort，只把扫描方向设成升/降
  （主键唯一，后面的排序列永远分不出胜负）；有 LIMIT 时 Sort 退化成 TopN
  （`n = OFFSET + LIMIT`）。

`UPDATE`/`DELETE` 没有 WHERE 时链就是 `FullScan`，绝不能当成"扫零行"
（静默少写数据是最危险的 bug），这一条有专门的测试守着。

`Planner::plan()` 返回 `std::expected<std::unique_ptr<PlanNode>, PlanError>`：
`PlanNode` 是抽象基类，按值返回会切片，原签名无法编译。

### 4. 成本模型（`planner/cost.h`）—— **占位性质**

这一步只是为了把"统计 → 成本 → 选计划"这个环节真实地放进链路，不追求准确：

- `Cost{startup, total}` 两个数：LIMIT 只关心 startup，其余关心 total；
- **目前唯一的决策点**：稀疏点集要不要下推（`optimizer.cpp`）。
  `k` 个点查 ≈ `k*(seek + 1 行)`；全表扫 + 过滤 ≈ `N*(扫一行 + 过一遍谓词)`；
  点集相对表太大时退回全表扫 —— **此时必须把主键谓词放回 `remaining_filter`**，
  否则会得到"少扫了但结果不对"的计划（成本模型不允许影响正确性）。
- 常数是相对量（扫一行 = 1），`seek = 10`：约 `k > N/5` 时点集退化。
- 统计只有一个数：表的行数。它来自 `@system/tablestats`（session 在写语句后维护，
  `sql_types` 的 Catalog 接口不用改 —— planner 通过 `StatsProvider` 这个
  `std::function` 拿统计）。**统计缺失时不做决策**，退回纯规则行为。
- `EXPLAIN` 打印 `cost=startup..total`，`EXPLAIN ANALYZE` 打印实际行数/耗时 ——
  两栏对照才能判断模型准不准。
