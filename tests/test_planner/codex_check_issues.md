# planner 模块实现记录（QueryRewriter / Optimizer / Planner）

审查范围：`planner/{planner_defs.h, rewriter.h, optimizer.h, plan.h, planner.h}`，
参考实现 `planner/old/{condition.cpp, scan_desc.h}`（废弃代码，不编译）。

结论：三个类按接口补齐并跑通；接口上有 6 处必须修改（下面第一节逐条列出），
设计上定了 4 条契约（候选集合、NULL 策略、exclude_keys 定位、重写器边界）。
实现过程中自查出 2 个真实缺陷（见第五节）。

验证：`cmake --build build -j4 && ctest --test-dir build`，
4/4 套件通过（sql_types / parser / statement / planner），
`planner` 下 51 用例 / 250 断言 / 0 失败；新增代码用
`-Wall -Wextra -Wpedantic` 单独编译 0 告警。

---

## 一、接口硬伤（必须改，已改）

| 位置 | 问题 | 处理 |
|------|------|------|
| `plan.h:5` | `#include "planner/plan_defs.h"` —— 该文件不存在（实际叫 `planner_defs.h`），一行都编译不过 | 改为 `planner/planner_defs.h` |
| `planner.h:13` | `std::expected<PlanNode, PlanError> plan(OptimizedQuery)`：`PlanNode` 是抽象基类，按值返回必然切片且**无法编译** | 改为 `std::expected<std::unique_ptr<PlanNode>, PlanError>` |
| `plan.h` | `PlanType` 有 `DROP_DATABASE / CREATE_TABLE / DROP_TABLE / USE_DATABASE`，但只有 `CreateDatabasePlan` 一个 DDL 节点类 | 补齐 4 个节点类 |
| `optimizer.h:18` | `OptimizedQuery` 无默认构造（`sql::Query` 显式删除了默认构造），而实现里必然要先造对象再填字段 | 加 `explicit OptimizedQuery(sql::Query)` + move 语义（不碰 `Query`） |
| `optimizer.h:45` | `PrimaryKeyExtractResult` 只有两棵条件树，`convert_to_key_range()` 拿不到列类型（编码 NULL/±∞ 需要）和库/表名 | 补 `db / table / pk_column / pk_type` 四个字段 |
| `optimizer.h:61` | `convert_to_key_range(const PrimaryKeyExtractResult&)` 造不出 `OptimizedQuery`（后者要持有 `Query` 的所有权） | 签名加 `const sql::Query& query` |
| `rewriter.h:40` | 文件结尾写 `}  // namespace sql`，实际打开的是 `namespace plan` | 修正；同时补 `<expected>` 与 `sql_types/condition.h` |
| `optimizer.h` | 用了 `std::expected` / `std::optional` 但没包含头文件（靠传递包含能过） | 补 `<expected> <optional> <string> <vector>` |

补的接口（additive，不改原方法）：

- `rewriter.h`：`sql::ConditionPtr rewrite_condition_tree(const sql::Condition*)`
  —— 流水线的唯一入口，`QueryRewriter` 与 `Optimizer` 共用，避免两份实现。
- `rewriter.h`：`sql::Query clone_query(const sql::Query&)` / `query_where(...)`
  —— `Query` 是 move-only，重写与优化都要"复制一份再改"。
- `optimizer.h`：`OptimizedQuery::is_all_scan() / is_empty_scan() / needs_index_scan() / point_value() / to_string()`
  —— 把候选集合的语义固定下来，调用方不必猜。

## 二、设计契约（本轮定案）

### 1. 候选集合的唯一表示

`ranges`（区间并集）与 `keys`（点集并集）是**候选**，`exclude_keys` 是
**要跳过的点**：

```
候选 = (⋃ ranges) ∪ keys   再跳过 exclude_keys
```

规范形式（`optimize()` 的输出保证）：

| 情况 | 表示 |
|------|------|
| 无主键条件（全表） | `ranges = {KeyRange::all()}`、`keys` 空 |
| 条件恒假（矛盾） | `ranges = {KeyRange::empty()}`、`keys` 空 |
| 纯点集（IN 列表） | `ranges` 空、`keys` 非空 |
| 恰好一个点 | `keys = {v}`、`is_point_query = true` |

没有这条约定，"ranges 空" 就同时可能表示"无约束"和"空集合"，执行器只能靠
猜。旧 `ScanDescriptor` 是靠"全表扫描也必须显式塞一个 `range_set = {all()}`
"来回避的，这里把同样的意思写成不变量 + `is_all_scan()/is_empty_scan()`。

### 2. NULL 策略（对齐 `sql_types/sql_truth.h`）

| 谓词 | 候选空间 | 是否留在 `remaining_filter` |
|------|----------|------------------------------|
| `id = 5`、`id > 5`、`id <= 5`… | 不含 NULL 的区间 | 否（完全下推） |
| `id IS NULL` | `null_only(type)` 单点 | 否 |
| `id IS NOT NULL` | `non_null(type)` | 否 |
| `id = NULL`（字面量 NULL） | 空候选 | 否 |
| `id IN (1, NULL)` | 只含 `1` | 否 |
| `id != 5`、`id NOT IN (…)` | 全表 + `exclude_keys` | **是**（正确性靠它） |

两个要点：

- **不用 `KeyRange::eq(NULL)`**。`key_range.h` 把 `eq(NULL)` 定义成"等价于
  IS NULL"（宽松解释），而 `id = NULL` 在 SQL 里恒为 UNKNOWN。Optimizer 对
  "右侧是 NULL 字面量" 单独判空集，避免 `WHERE id = NULL` 扫出 NULL 行。
- `NOT IN` 列表里带 NULL 时（`id NOT IN (1, NULL)`）整条谓词对任何行都是
  UNKNOWN。这里不做特殊处理：`exclude_keys` 只收 `1`，行由
  `remaining_filter` 全部过滤掉，结果仍然正确（性质测试覆盖）。

### 3. exclude_keys 是提示，不是契约

`<>` / `NOT IN` 不可用索引定位，所以候选空间仍是全表；把它们记进
`exclude_keys` 供扫描器"少扫几个点"，同时**原样保留在 `remaining_filter`**
里。这样执行器即使完全忽略 `exclude_keys`，结果也不会错 —— 提示类优化
不该承担正确性。

（与之相对，`ranges`/`keys` 是契约：`id = 5` 时 `remaining_filter == nullptr`，
执行器必须按 `keys` 扫。）

### 4. 重写器不做区间算术

旧 `condition.cpp` 的 `normalize_boundary` 用 `value+1` 把 `>`/`<=` 规范化，
在 `INT64_MAX` 上溢出，而且按字符串比较把 `id > 'x'` 当数值处理。新实现：
重写器只做**结构**化简，区间代数（NULL、±∞、开闭、相邻、合并）统一交给
`KeyRange`；Optimizer 把每个主键叶子翻成 `KeyRange` 后做交/并。

同时不再使用 `1=1` / `1=0` 常量哨兵：矛盾条件原样保留，由 Optimizer 转成
空区间；"真"用 `nullptr`（无条件）表达，不需要第三种表示。

## 三、实现内容

新增文件：

| 文件 | 内容 |
|------|------|
| `planner/rewriter.cpp` | `pushdown_not / flatten / fold_constants / deduplicate / simplify` + `clone_query` / `query_where` |
| `planner/optimizer.cpp` | 主键抽取（`split_primary_key`）、区间集合代数（`normalize / intersect / unite`）、`range_of_condition`、`collect_exclusions`、分派与错误路径 |
| `planner/plan.cpp` | 9 个计划节点的 `to_string()` |
| `planner/planner.cpp` | 规则式 `plan()` |

Rewriter 规则：

- NOT 下推：De Morgan + `flip_compare_op`；`NOT (x IN …)` 直接用
  `InCondition::is_not_in` 表达（三值语义一致），不可翻转的保留 NOT。
- 扁平化：`a AND (b AND c)` → `((a AND b) AND c)`，让去重/吸收看到同层兄弟。
- 常量折叠：IN 列表去重、单元素 IN 退化成 `=`/`!=`。
- 去重：同层结构相等（类型 + `to_string`）只留一个。
- 吸收律：`A OR (A AND B) → A`、`A AND (A OR B) → A`。
- OR-of-EQ → IN：同列 ≥2 个 `=` 合并为 `IN`；不同列不合并。

Optimizer 规则：

- 表/库解析 → 表无主键时退化为全表扫描（不算错误）。
- `AND` 两侧都能下推才下推；`OR` 只有两侧都是"纯主键条件"才下推，
  否则整棵 OR 退回过滤（只下推一支会少扫行）。
- 区间：`normalize`（丢空、排序、合并重叠/相邻）+ `intersect` + `unite`；
  空结果统一写成 `{KeyRange::empty()}`。
- 点区间进 `keys`，其余进 `ranges`。

Planner 规则：`is_all_scan()` → `FullScan`，否则 `IndexScanPlan`；
写语句与 DDL 一一对应；`UPDATE`/`DELETE` 无 WHERE = 全表（不做"零行"）。

## 四、测试（tests/test_planner/，51 用例）

| 文件 | 用例数 | 覆盖 |
|------|--------|------|
| `test_rewriter.cpp` | 13 | NOT 下推/De Morgan/双重否定/IS NULL/IN 翻转、扁平化、去重、吸收律、OR→IN、IN 折叠、DDL/INSERT 原样返回、**原 Query 不被改动** |
| `test_optimizer.cpp` | 27 | 点查/IN/OR/区间/两个半区间/单值区间、NULL 六种谓词、矛盾→空扫描、`<>`/`NOT IN` 提示、非主键过滤、OR 不可下推、无 WHERE（SELECT/UPDATE/DELETE）、无主键表、字符串主键的范围与字节序、错误码（CATALOG_NOT_OPEN / TABLE_NOT_FOUND） |
| `test_planner.cpp` | 11 | SELECT 的两种扫描、空扫描仍有计划、INSERT/UPDATE/DELETE、5 个 DDL/USE 计划、`UPDATE` 无 WHERE 是全表 |

其中 `Optimizer.PlanKeepsExactlyTheRowsThePredicateKeeps` 是**性质测试**：
对 29 条谓词 × 8 个样本值（含 NULL），比较
"计划保留该行" 与 "原谓词保留该行"（用 `sql_types/sql_truth.h` 的
三值求值），专门盯住 NULL 策略与下推是否漏行。这一条在开发中立刻抓到了
第五节的两个缺陷。

## 五、实现过程中自查出并修掉的问题（新代码，非旧代码）

1. `simplify_and/simplify_or` 的吸收律先 `std::move` 再继续用兄弟节点比较，
   搬移后的 `unique_ptr` 被置空 → `tree_contains(nullptr)` 段错误。
   改为"先判定（`vector<bool> absorbed`）、后搬移"。重写器套件曾整体 SIGSEGV。
2. `convert_to_key_range` 在主键条件**成功**下推的分支里忘了拷 `remaining`
   → `id = 5 AND name = 'x'` 变成 `keys={5}` + 空过滤，等于放宽了条件。
   由性质测试发现，补上 `out.remaining_filter = clone(remaining)`。

两处都说明：这种"优化后条件必须等价"的不变量，靠断言
`remaining_filter != nullptr` 是不够的，必须与谓词语义逐值对照。

## 六、遗留 / 下一步

- **执行器接口**：`PlanNode` 只描述"怎么做"，`ranges/keys/exclude_keys`
  的迭代状态（离散区间拼成连续 `next()`）留给执行层；`run_tests` 里还没有
  执行器用例。
- **Filter / Projection 节点**：readme 提到 Scan → Filter → Projection，
  现在的 `plan.h` 只有 Scan/写节点，过滤条件挂在 `OptimizedQuery` 上。
  等执行器落地后再决定是否拆出独立节点（拆了就要处理"每个节点都要过滤"
  与"过滤只做一次"的区别）。
- **区间代数的归属**：`normalize/intersect/unite` 目前是 optimizer.cpp 的
  文件内函数。如果 storage 侧也要用，应下沉成 `KeyRange::unite_all/intersect_all`
  之类的接口，避免第二份实现。
- **LIKE 前缀**：字符串主键的 `LIKE 'abc%'` 本可以下推成前缀区间，现在按
  "不可索引" 退回过滤。
- **复合主键 / KeyPrefix**：仍不支持（按约定），多主键表的优化器会当成
  无主键全表扫描。
- **代价模型**：按 readme 约定不做；当前规则只看"候选空间是否被收窄"。
- `storage/range_convert.h` 依旧引用已删除的老接口，接上层时按
  `StrKeyRange{start,end}` 重写。

---

## 七、Planner 重构：搭真正的 volcano 链（2026-09-12 追加）

起因（review 意见）："planner 现在的设计有点粗糙，没有针对 statement 构建
volcano 链，没有体现 builder 的意义，是把活给执行层吗。" —— 成立，具体三处：

1. `UPDATE`/`DELETE` 是"叶子大节点"，执行器得自己写"扫—滤—改"的循环，
   等于把 plan 层的策略推给执行层；
2. "离散区间 → 连续行流"的状态机焊在扫描节点内部，没有表达成算子；
3. 树的形状是在 `plan_select` 里手写 if/else 拼的，公共部分没有复用。

（边界也要说清楚：算子"怎么干活"本来就属于执行层；问题在于结构没有在
计划里表达出来。）

### 1. 算子与链

```
SELECT:  Project? -> Limit? -> Sort/TopN? -> Filter? -> RangeUnion? -> FullScan/IndexScan
UPDATE:  Update   -> Limit? -> Sort?     -> Filter? -> RangeUnion? -> FullScan/IndexScan
DELETE:  Delete   -> Limit? -> Sort?     -> Filter? -> RangeUnion? -> FullScan/IndexScan
INSERT:  Insert（child 为空 = 行源是语句里的 VALUES；将来 INSERT ... SELECT 挂子链）
DDL/USE: 叶子
```

`planner.cpp` 的 `build_scan_chain(OptimizedQuery&, ascending)` 是三个语句
共用的那一段：

| 候选空间 | 扫描算子 |
|----------|----------|
| 单区间 `is_all()`、无跳点 | `FullScan` |
| 单区间（含点区间）、无跳点 | `IndexScanPlan`（点查询 = 区间 `is_point()`） |
| 多区间，或有跳点（`<>` / `NOT IN`） | `RangeUnionPlan`（持有区间数组 + 跳点） |
| 条件恒假（`{empty()}`） | `IndexScanPlan`（空区间，扫不到行） |

### 2. 三个决定（按推荐实现）

1. **`RangeUnion` 持有区间数组**，不是 N 个 `IndexScan` 子节点：
   `IN (1..1000)` 不会变成 1000 个节点，节点数 O(1)；多区间拼接的状态机
   住在算子内部，可以单独测试（正/反向、空区间、相邻区间、LIMIT 早停）。
2. **`exclude_keys` 由 `RangeUnion` 消费**（归并式跳过），`Filter` 里的谓词
   仍然兜底 —— 提示类优化不承担正确性。
3. **`Insert` 带 child（可为空）**：VALUES 形态 child 为空（行源在语句里），
   为 `INSERT ... SELECT` 留位。

### 3. 顺便定下的两件事

- **建树时"搬家"**：`ranges` / `exclude_keys` / `remaining_filter` 从
  `OptimizedQuery` 移进对应算子，树里只有一份真相；扫描算子改用轻量的
  `TableRef{db, table, primary_key, primary_key_type}` 定位表。
  （`sql::Query` 是 move-only，整棵树里只有写语句/DDL 节点持有它。）
- **`OptimizedQuery::primary_key_type`**（新增字段）：由 Optimizer 填入主键列
  的逻辑类型。不能从区间边界值上"猜"——`range` 里的值类型可能是 BIGINT
  而列声明是 INT，同族但不等价。

### 4. 又踩一次的坑（已写进注释）

`std::make_unique<UpdatePlan>(std::move(query), build_scan_chain(query))`
这种写法里**函数实参求值顺序不确定**：`build_scan_chain` 可能读到
moved-from 的语句。现在一律先建链、再 move：

```cpp
std::unique_ptr<PlanNode> chain = build_scan_chain(query, true);
return std::make_unique<UpdatePlan>(std::move(query), std::move(chain));
```

### 5. 测试（Planner 24 用例）

- 扫描算子选择：FullScan / 单区间 IndexScan / 点查询 / `RangeUnion`（3 个点）/
  `id != 5` 的跳点落在 `RangeUnion` 上 / 恒假条件仍是可执行的扫描。
- 链的形状：`Project -> Limit -> TopN -> Filter -> FullScan` 逐层断言，外加
  `plan_tree_to_string` 的关键片段。
- Sort 消除：`ORDER BY id`、`ORDER BY id DESC`（反向扫描）、
  `ORDER BY id, age DESC`（主键唯一，后续键不定胜负）。
- 非主键排序：`ORDER BY age` 建 Sort；`LIMIT 10` 退化成 TopN(n=10)；
  `LIMIT 10 OFFSET 5` → TopN(n=15)。
- **写语句复用扫描链**：`UPDATE ... WHERE id = 3 AND name = 'x'` 的形状是
  `Update -> Filter -> IndexScan(点)`；`UPDATE ...`（无 WHERE）是
  `Update -> FullScan`；同一 WHERE 下 `SELECT` 与 `DELETE` 的扫描算子
  `to_string()` 完全相同（共享 `build_scan_chain` 的直接证据）。
- INSERT（child 为空）与 5 个 DDL/USE 计划。
