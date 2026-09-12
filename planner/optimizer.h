// optimizer.h
#ifndef QUERY_OPTIMIZER_H
#define QUERY_OPTIMIZER_H

#include <expected>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "planner/planner_defs.h"
#include "sql_types/catalog.h"
#include "sql_types/identifier.h"
#include "sql_types/key_range.h"
#include "sql_types/key_set.h"
#include "sql_types/query.h"

namespace plan {

// ============================================================
// OptimizedQuery：优化器交给 Planner 的"扫描空间 + 残余过滤"
//
// 扫描空间只有一个概念：**有序、互不相交的区间数组** ranges。
//   - 点查询就是退化区间 [v, v]（执行器可以退化成一次 Get）；
//   - 数组恒按 low_key 升序（= 主键升序），相邻/重叠的区间已经合并，
//     所以顺序迭代 ranges 得到的行就是**全局主键有序**的
//     —— LIMIT/OFFSET 要的"全局一致的顺序"由此保证；
//   - 整数/时间类型里"连续的点"（IN (1,2,3)、= 1 OR = 2）会合并成一段，
//     减少 seek 次数；
//   - 降序不用另一套表示：ORDER BY pk DESC 由游标**反向迭代**实现
//     （见 plan::Ordering）。
//
// 候选集合的**规范形式**（optimize() 的输出保证满足，唯一表示）：
//     无主键条件（全表）  -> ranges = { KeyRange::all() }
//     条件恒假（矛盾）    -> ranges = { KeyRange::empty() }
//     恰好一个点          -> ranges = { [v,v] }, is_point_query = true
//   is_empty_scan()（扫不到行）与 is_all_scan()（全表）互斥，
//   "区间数组为空" 一律按空候选处理。
//
// exclude_keys 是**排序无关的跳点集**：`<>` / `NOT IN` 里的主键点，
// 扫描时跳过即可（跳过不会打乱顺序）。它只是提示，对应的谓词同时保留在
// remaining_filter 里，所以执行器忽略它也依然正确。
//
// 关于过滤条件：
//   执行器**必须**把 remaining_filter 应用到每一行（WHERE 只保留 TRUE）。
//   query.where 保存的是重写前的原始条件，语义与重写后等价、AND 幂等，
//   因此执行器多套一层也不会出错。
// ============================================================
struct OptimizedQuery {
  sql::Query query;
  sql::Identifier db;
  sql::Identifier table;
  sql::Identifier primary_key;
  // 主键列的逻辑类型（编码 key / 生成扫描边界用；无主键或 DDL 时是
  // UNKNOWN_TYPE）。注意不是从区间边界值上"猜"出来的：
  // range 里的值类型可能是 BIGINT 而列声明是 INT，两者同族但不等价。
  sql::DataType primary_key_type = sql::DataType::UNKNOWN_TYPE;
  std::vector<sql::KeyRange> ranges;
  sql::KeySet exclude_keys;
  bool is_point_query = false;
  sql::ConditionPtr remaining_filter;

  // Query 没有默认构造（type_ 由 variant 推导，不允许"空 Query"），
  // 所以 OptimizedQuery 也没有默认构造：必须带着语句一起构造。
  explicit OptimizedQuery(sql::Query stmt) : query(std::move(stmt)) {}
  OptimizedQuery(OptimizedQuery &&) noexcept = default;
  OptimizedQuery &operator=(OptimizedQuery &&) noexcept = default;
  OptimizedQuery(const OptimizedQuery &) = delete;
  OptimizedQuery &operator=(const OptimizedQuery &) = delete;
  ~OptimizedQuery() = default;

  // 候选空间就是整张表（且没有要跳过的点）-> FullScan
  bool is_all_scan() const {
    return ranges.size() == 1 && ranges[0].is_all() && exclude_keys.empty();
  }

  // 条件恒假：没有任何行会被扫描到
  bool is_empty_scan() const {
    for (const auto &r : ranges) {
      if (!r.is_empty()) {
        return false;
      }
    }
    return true;
  }

  // 候选空间是否被主键条件收窄（决定 IndexScanPlan / FullScan）
  bool needs_index_scan() const { return !is_all_scan(); }

  // is_point_query 时返回那个点（NULL 点是合法的：IS NULL）
  std::optional<sql::Value> point_value() const {
    if (ranges.size() == 1 && ranges[0].is_point()) {
      return ranges[0].point_value();
    }
    return std::nullopt;
  }

  // 调试用：table pk=id ranges=[...] keys={...} exclude={...} filter=...
  std::string to_string() const;
};

class Optimizer {
public:
  explicit Optimizer(const sql::Catalog &catalog) : catalog_(catalog) {}

  // 根据 query 类型分派：只有 DML 需要"主键抽取 -> 重写 -> 转 KeyRange"，
  // DDL/USE 不带扫描空间（ranges = {all()} 的规范形式）。
  // extract_primary_key_condition -> rewrite_primary_key_condition ->
  // convert_to_key_range
  std::expected<OptimizedQuery, PlanError> optimize(const sql::Query &query);

private:
  std::expected<OptimizedQuery, PlanError>
  optimize_select(const sql::Query &query);
  std::expected<OptimizedQuery, PlanError>
  optimize_update(const sql::Query &query);
  std::expected<OptimizedQuery, PlanError>
  optimize_delete(const sql::Query &query);
  std::expected<OptimizedQuery, PlanError>
  optimize_insert(const sql::Query &query);

  // 主键抽取的中间结果
  //   pk_cond AND remaining = 原始 WHERE（条件树按值语义相等）
  // 另外带上目标表与主键的元信息：convert_to_key_range 需要
  //   (1) 列类型才能把 NULL / ±∞ 编码成 key；
  //   (2) 库/表名才能填进 OptimizedQuery。
  struct PrimaryKeyExtractResult {
    sql::ConditionPtr pk_cond;   // 主键条件树
    sql::ConditionPtr remaining; // 剩余条件树
    sql::Identifier db;
    sql::Identifier table;
    sql::Identifier pk_column; // 表无主键时为空
    sql::DataType pk_type = sql::DataType::UNKNOWN_TYPE;

    bool has_primary_cond() const { return pk_cond != nullptr; }
    bool has_remaining_cond() const { return remaining != nullptr; }
    bool has_primary_key() const { return !pk_column.empty(); }
  };

  // 尝试从原条件树中提取出主键条件：
  //   - 能提取 -> pk_cond / remaining 各就各位；
  //   - 提取不出（约束不成立等）-> 返回错误；
  //   - 表无主键 -> pk_cond 为空，remaining 是整棵条件（不算错误）。
  std::expected<PrimaryKeyExtractResult, PlanError>
  extract_primary_key_condition(const sql::Query &query);

  // 用 rewriter 再次重写简化两棵树
  std::expected<PrimaryKeyExtractResult, PlanError>
  rewrite_primary_key_condition(const PrimaryKeyExtractResult &pk_result);

  // 将主键条件树转换为 KeyRange/KeySet
  // （需要原 query：OptimizedQuery 持有 query 的所有权）
  std::expected<OptimizedQuery, PlanError>
  convert_to_key_range(const sql::Query &query,
                       const PrimaryKeyExtractResult &pk_result);

private:
  const sql::Catalog &catalog_; // only for lookup, so make const constraint.
};

} // namespace plan

#endif // QUERY_OPTIMIZER_H
