// optimizer.cpp
//
// Optimizer：从 WHERE 里把**主键条件**抽出来，翻译成 key 空间的候选集合，
// 剩下的条件作为 filter 留给执行器。
//
// 三个步骤（optimizer.h 里声明的私有方法）：
//   1) extract_primary_key_condition
//        解析表/主键元信息，把 WHERE 拆成 pk_cond + remaining
//   2) rewrite_primary_key_condition
//        用 rewriter 再简化这两棵树（NOT 下推后才会出现 pk<> / pk NOT IN）
//   3) convert_to_key_range
//        把 pk_cond 翻成 KeyRange / KeySet，并生成 OptimizedQuery
//
// NULL 策略（sql_types/sql_truth.h 的三值逻辑）：
//   - 比较谓词（= <> < <= > >=）永远不匹配 NULL：除 IS NULL / IS NOT NULL
//     外，比较产生的区间都不含 NULL（用
//     KeyRange::gt/ge/lt/le/point/non_null）。
//   - 谓词右侧是 NULL 时（如 id = NULL）：任何比较结果都是 UNKNOWN，
//     一行都命中不了 -> 空候选（**不能**用 KeyRange::eq(NULL)，
//     那是"= NULL 等价于 IS NULL"的宽松解释）。
//   - IN 列表里的 NULL 不贡献任何行；整张表都没有可命中的值 -> 空候选。
//   - NOT IN / <> 不可索引：候选仍是全表（或已有区间），把它们记到
//     exclude_keys 作为扫描提示，同时保留在 remaining_filter 里 ——
//     正确性只依赖 remaining_filter，exclude_keys 只是"少扫几个点"。
#include "optimizer.h"

#include <algorithm>
#include <optional>
#include <string>
#include <utility>

#include "planner/rewriter.h"
#include "sql_types/compare_op.h"
#include "sql_types/condition_types.h"

namespace plan {
namespace {

using sql::CompareOp;
using sql::Condition;
using sql::ConditionPtr;
using sql::Identifier;
using sql::KeyRange;
using sql::Value;

// ---- 区间排序：-∞ 最小，+∞ 最大 ----
bool range_less(const KeyRange &a, const KeyRange &b) {
  const auto a_low = a.low_key();
  const auto b_low = b.low_key();
  if (a_low.has_value() != b_low.has_value()) {
    return !a_low.has_value(); // 无下界 = -∞，排前面
  }
  if (a_low.has_value() && *a_low != *b_low) {
    return *a_low < *b_low;
  }
  const auto a_high = a.high_key();
  const auto b_high = b.high_key();
  if (a_high.has_value() != b_high.has_value()) {
    return a_high.has_value(); // 无上界 = +∞，排后面
  }
  if (a_high.has_value()) {
    return *a_high < *b_high;
  }
  return false;
}

// 规范化：丢掉空区间、排序、合并重叠或相邻的区间
std::vector<KeyRange> normalize_ranges(std::vector<KeyRange> ranges) {
  std::vector<KeyRange> sorted;
  for (const auto &r : ranges) {
    if (!r.is_empty()) {
      sorted.push_back(r);
    }
  }
  std::sort(sorted.begin(), sorted.end(), range_less);

  std::vector<KeyRange> merged;
  for (const auto &r : sorted) {
    if (!merged.empty() &&
        (merged.back().overlaps(r) || merged.back().is_adjacent(r))) {
      auto united = merged.back().unite(r);
      if (united.size() == 1) {
        merged.back() = united.front();
        continue;
      }
    }
    merged.push_back(r);
  }
  return merged;
}

// 交集：两两求交后规范化（区间数很少，O(n*m) 足够）
std::vector<KeyRange> intersect_ranges(const std::vector<KeyRange> &a,
                                       const std::vector<KeyRange> &b) {
  std::vector<KeyRange> result;
  for (const auto &lhs : a) {
    for (const auto &rhs : b) {
      KeyRange inter = lhs.intersect(rhs);
      if (!inter.is_empty()) {
        result.push_back(inter);
      }
    }
  }
  return normalize_ranges(std::move(result));
}

// 并集
std::vector<KeyRange> unite_ranges(const std::vector<KeyRange> &a,
                                   const std::vector<KeyRange> &b) {
  std::vector<KeyRange> result = a;
  result.insert(result.end(), b.begin(), b.end());
  return normalize_ranges(std::move(result));
}

// 把"连续的点"合并成一段区间：{1, 2, 3} -> [1, 3]。
// 只对整数/时间类型做（它们的值在 key 空间里等距，"相邻"有定义）；
// 字符串没有"下一个值"的概念，保持一个个点区间。
// 目的是减少 seek 次数：IN (1..100) 从 100 次 seek 变成 1 次顺序扫。
std::vector<KeyRange> coalesce_adjacent_points(std::vector<KeyRange> ranges,
                                               sql::DataType pk_type) {
  if (!sql::is_integer(pk_type) && !sql::is_temporal(pk_type)) {
    return ranges;
  }
  std::vector<KeyRange> result;
  size_t i = 0;
  while (i < ranges.size()) {
    if (!ranges[i].is_point()) {
      result.push_back(ranges[i]);
      ++i;
      continue;
    }
    const Value start = ranges[i].point_value();
    if (start.is_null()) {  // NULL 是单点，不能和后一个值连起来
      result.push_back(ranges[i]);
      ++i;
      continue;
    }
    Value end = start;
    size_t last = i;
    while (last + 1 < ranges.size() && ranges[last + 1].is_point()) {
      const Value next = ranges[last + 1].point_value();
      if (next.is_null() || !next.is_int() || end.as_int() == INT64_MAX ||
          next.as_int() != end.as_int() + 1) {
        break;
      }
      end = next;
      ++last;
    }
    result.push_back(last > i ? KeyRange::closed(start, end) : ranges[i]);
    i = last + 1;
  }
  return result;
}

// 主键条件 -> 候选区间集合。
// 返回 nullopt 表示"这棵子树翻译不出来"（调用方退回全表 + 过滤）。
std::optional<std::vector<KeyRange>> range_of_condition(const Condition &node,
                                                        sql::DataType pk_type) {
  switch (node.type()) {
  case sql::ConditionType::COMPARE: {
    const auto &cmp = static_cast<const sql::CompareCondition &>(node);
    if (cmp.op() == CompareOp::IS_NULL) {
      return std::vector<KeyRange>{KeyRange::null_only(pk_type)};
    }
    if (cmp.op() == CompareOp::IS_NOT_NULL) {
      return std::vector<KeyRange>{KeyRange::non_null(pk_type)};
    }
    if (!sql::is_indexable(cmp.op())) {
      return std::nullopt; // NE / LIKE：不可索引
    }
    const Value &v = cmp.value();
    if (v.is_null()) {
      // 与 NULL 比较恒为 UNKNOWN：一行都命中不了
      return std::vector<KeyRange>{KeyRange::empty(pk_type)};
    }
    switch (cmp.op()) {
    case CompareOp::EQ:
      return std::vector<KeyRange>{KeyRange::point(v)};
    case CompareOp::GT:
      return std::vector<KeyRange>{KeyRange::gt(v)};
    case CompareOp::GE:
      return std::vector<KeyRange>{KeyRange::ge(v)};
    case CompareOp::LT:
      return std::vector<KeyRange>{KeyRange::lt(v)};
    case CompareOp::LE:
      return std::vector<KeyRange>{KeyRange::le(v)};
    default:
      return std::nullopt;
    }
  }
  case sql::ConditionType::IN: {
    const auto &in = static_cast<const sql::InCondition &>(node);
    if (in.is_not_in()) {
      return std::nullopt; // NOT IN：交给 exclude_keys + 过滤
    }
    std::vector<KeyRange> result;
    for (const auto &v : in.values()) {
      if (!v.is_null()) { // IN 里的 NULL 不命中任何行
        result.push_back(KeyRange::point(v));
      }
    }
    if (result.empty()) {
      return std::vector<KeyRange>{KeyRange::empty(pk_type)};
    }
    return result;
  }
  case sql::ConditionType::AND: {
    const auto &n = static_cast<const sql::AndCondition &>(node);
    auto left = range_of_condition(*n.left(), pk_type);
    auto right = range_of_condition(*n.right(), pk_type);
    if (!left.has_value() || !right.has_value()) {
      return std::nullopt;
    }
    return intersect_ranges(*left, *right);
  }
  case sql::ConditionType::OR: {
    const auto &n = static_cast<const sql::OrCondition &>(node);
    auto left = range_of_condition(*n.left(), pk_type);
    auto right = range_of_condition(*n.right(), pk_type);
    if (!left.has_value() || !right.has_value()) {
      return std::nullopt;
    }
    return unite_ranges(*left, *right);
  }
  default:
    return std::nullopt;
  }
}

ConditionPtr join_and(ConditionPtr left, ConditionPtr right) {
  if (!left) {
    return right;
  }
  if (!right) {
    return left;
  }
  return sql::make_and(std::move(left), std::move(right));
}

ConditionPtr join_or(ConditionPtr left, ConditionPtr right) {
  if (!left) {
    return right;
  }
  if (!right) {
    return left;
  }
  return sql::make_or(std::move(left), std::move(right));
}

// 把拆分结果还原成一棵树（pk_cond AND remaining）
ConditionPtr rebuild_split(ConditionPtr pk, ConditionPtr remaining) {
  return join_and(std::move(pk), std::move(remaining));
}

struct SplitConditions {
  ConditionPtr pk_cond;
  ConditionPtr remaining;
};

// 递归拆分：能用于主键定位的进 pk_cond，其余进 remaining
SplitConditions split_primary_key(const Condition &node, const Identifier &pk) {
  if (node.is_compare()) {
    const auto &cmp = static_cast<const sql::CompareCondition &>(node);
    const bool usable = cmp.column() == pk && (sql::is_indexable(cmp.op()) ||
                                               sql::is_null_op(cmp.op()));
    if (usable) {
      return {cmp.clone(), nullptr};
    }
    return {nullptr, cmp.clone()};
  }
  if (node.is_in()) {
    const auto &in = static_cast<const sql::InCondition &>(node);
    if (in.column() == pk && !in.is_not_in()) {
      return {in.clone(), nullptr};
    }
    return {nullptr, in.clone()};
  }
  if (node.is_and()) {
    const auto &n = static_cast<const sql::AndCondition &>(node);
    SplitConditions left = split_primary_key(*n.left(), pk);
    SplitConditions right = split_primary_key(*n.right(), pk);
    return {join_and(std::move(left.pk_cond), std::move(right.pk_cond)),
            join_and(std::move(left.remaining), std::move(right.remaining))};
  }
  if (node.is_or()) {
    const auto &n = static_cast<const sql::OrCondition &>(node);
    SplitConditions left = split_primary_key(*n.left(), pk);
    SplitConditions right = split_primary_key(*n.right(), pk);
    // 只有两支都是"纯主键条件"时，OR 整体才是主键条件；
    // 否则整棵 OR 退回过滤（只下推一支会少扫行）
    if (!left.remaining && !right.remaining) {
      return {join_or(std::move(left.pk_cond), std::move(right.pk_cond)),
              nullptr};
    }
    return {nullptr, sql::make_or(rebuild_split(std::move(left.pk_cond),
                                                std::move(left.remaining)),
                                  rebuild_split(std::move(right.pk_cond),
                                                std::move(right.remaining)))};
  }
  // NOT / 其它：整棵退回过滤（NOT 已在 rewriter 里下推掉）
  return {nullptr, node.clone()};
}

// 从 remaining 的 AND 主干上收集主键排除点（<> / NOT IN）。
// 只在 AND 主干上走：OR 分支里的 <> 不是全局排除，不能进 exclude_keys。
void collect_exclusions(const Condition *node, const Identifier &pk,
                        sql::KeySet &out) {
  if (node == nullptr) {
    return;
  }
  if (node->is_and()) {
    const auto &n = static_cast<const sql::AndCondition &>(*node);
    collect_exclusions(n.left(), pk, out);
    collect_exclusions(n.right(), pk, out);
    return;
  }
  if (node->is_compare()) {
    const auto &cmp = static_cast<const sql::CompareCondition &>(*node);
    if (cmp.column() == pk && cmp.op() == CompareOp::NE &&
        !cmp.value().is_null()) {
      out.add(cmp.value());
    }
    return;
  }
  if (node->is_in()) {
    const auto &in = static_cast<const sql::InCondition &>(*node);
    if (in.column() == pk && in.is_not_in()) {
      for (const auto &v : in.values()) {
        if (!v.is_null()) {
          out.add(v);
        }
      }
    }
  }
}

// 解析目标库/表：SELECT/UPDATE/DELETE/INSERT/CREATE TABLE/DROP TABLE 有表名
std::expected<std::pair<Identifier, Identifier>, PlanError>
resolve_target(const sql::Catalog &catalog, const sql::Query &query) {
  if (!catalog.is_open()) {
    return std::unexpected(
        PlanError(PlanErrorCode::CATALOG_NOT_OPEN, "catalog is not open"));
  }
  const Identifier *table = query.target_table();
  if (table == nullptr) {
    return std::unexpected(
        PlanError(PlanErrorCode::UNSUPPORTED_QUERY,
                  "query has no target table: " + query.to_string()));
  }
  Identifier db = catalog.current_database();
  if (db.empty()) {
    return std::unexpected(
        PlanError(PlanErrorCode::CATALOG_NOT_OPEN, "no database selected"));
  }
  if (table->empty()) {
    return std::unexpected(
        PlanError(PlanErrorCode::EMPTY_TABLE_NAME, "empty table name"));
  }
  return std::make_pair(db, *table);
}

} // namespace

// 上面匿名 namespace 里的 using 已随作用域结束；命名空间内继续用这几个别名
using sql::ConditionPtr;
using sql::Identifier;
using sql::KeyRange;

// ============================================================
// OptimizedQuery::to_string
// ============================================================
std::string OptimizedQuery::to_string() const {
  std::string s = table.empty() ? "?" : table.str();
  if (!primary_key.empty()) {
    s += " pk=" + primary_key.str();
  }
  s += " ranges=[";
  for (size_t i = 0; i < ranges.size(); ++i) {
    if (i > 0) {
      s += ", ";
    }
    s += ranges[i].to_string();
  }
  s += "]";
  if (!exclude_keys.empty()) {
    s += " exclude=" + exclude_keys.to_string();
  }
  if (is_point_query) {
    s += " point";
  }
  s += " filter=" + (remaining_filter ? remaining_filter->to_string() : "-");
  return s;
}

// ============================================================
// optimize 分派
// ============================================================
std::expected<OptimizedQuery, PlanError>
Optimizer::optimize(const sql::Query &query) {
  switch (query.type()) {
  case sql::QueryType::SELECT:
    return optimize_select(query);
  case sql::QueryType::INSERT:
    return optimize_insert(query);
  case sql::QueryType::UPDATE:
    return optimize_update(query);
  case sql::QueryType::DELETE:
    return optimize_delete(query);
  case sql::QueryType::CREATE_TABLE:
  case sql::QueryType::DROP_TABLE:
  case sql::QueryType::CREATE_DATABASE:
  case sql::QueryType::DROP_DATABASE:
  case sql::QueryType::USE_DATABASE: {
    // 非扫描语句：只填库/表名，不带扫描空间（规范形式：ranges = {all()}）
    OptimizedQuery out(clone_query(query));
    out.db = catalog_.current_database();
    if (const Identifier *table = query.target_table()) {
      out.table = *table;
    }
    if (const auto *create_db = query.create_database()) {
      out.db = create_db->database;
    }
    if (const auto *drop_db = query.drop_database()) {
      out.db = drop_db->database;
    }
    if (const auto *use_db = query.use_database()) {
      out.db = use_db->database;
    }
    out.ranges.push_back(KeyRange::all());
    return out;
  }
  default:
    return std::unexpected(
        PlanError(PlanErrorCode::UNSUPPORTED_QUERY,
                  "unsupported query: " + query.to_string()));
  }
}

std::expected<OptimizedQuery, PlanError>
Optimizer::optimize_select(const sql::Query &query) {
  auto extracted = extract_primary_key_condition(query);
  if (!extracted) {
    return std::unexpected(extracted.error());
  }
  auto rewritten = rewrite_primary_key_condition(*extracted);
  if (!rewritten) {
    return std::unexpected(rewritten.error());
  }
  return convert_to_key_range(query, *rewritten);
}

std::expected<OptimizedQuery, PlanError>
Optimizer::optimize_update(const sql::Query &query) {
  auto extracted = extract_primary_key_condition(query);
  if (!extracted) {
    return std::unexpected(extracted.error());
  }
  auto rewritten = rewrite_primary_key_condition(*extracted);
  if (!rewritten) {
    return std::unexpected(rewritten.error());
  }
  return convert_to_key_range(query, *rewritten);
}

std::expected<OptimizedQuery, PlanError>
Optimizer::optimize_delete(const sql::Query &query) {
  auto extracted = extract_primary_key_condition(query);
  if (!extracted) {
    return std::unexpected(extracted.error());
  }
  auto rewritten = rewrite_primary_key_condition(*extracted);
  if (!rewritten) {
    return std::unexpected(rewritten.error());
  }
  return convert_to_key_range(query, *rewritten);
}

std::expected<OptimizedQuery, PlanError>
Optimizer::optimize_insert(const sql::Query &query) {
  // INSERT 不读表，但仍要解析出库/表并确认表存在（顺便拿到主键类型），
  // 否则计划阶段会把"表不存在"漏到执行期
  auto target = resolve_target(catalog_, query);
  if (!target) {
    return std::unexpected(target.error());
  }
  const auto &[db, table] = *target;
  auto schema = catalog_.get_table_schema(db, table);
  if (!schema.has_value()) {
    return std::unexpected(PlanError(PlanErrorCode::TABLE_NOT_FOUND,
                                     "table not found: " + table.str()));
  }

  OptimizedQuery out(clone_query(query));
  out.db = db;
  out.table = table;
  if (schema->has_primary_key()) {
    out.primary_key = schema->primary_key_name();
    const sql::ColumnDef *pk = schema->primary_key_column();
    if (pk != nullptr) {
      out.primary_key_type = pk->type;
    }
  }
  out.ranges.push_back(KeyRange::all(out.primary_key_type));
  return out;
}

// ============================================================
// 1) 主键条件抽取
// ============================================================
std::expected<Optimizer::PrimaryKeyExtractResult, PlanError>
Optimizer::extract_primary_key_condition(const sql::Query &query) {
  auto target = resolve_target(catalog_, query);
  if (!target) {
    return std::unexpected(target.error());
  }
  const auto &[db, table] = *target;

  auto schema = catalog_.get_table_schema(db, table);
  if (!schema.has_value()) {
    return std::unexpected(PlanError(PlanErrorCode::TABLE_NOT_FOUND,
                                     "table not found: " + table.str()));
  }

  PrimaryKeyExtractResult result;
  result.db = db;
  result.table = table;

  // 先把条件重写成规范形式（NOT 下推、扁平化、去重……），
  // 这样 NOT(id = 5) 已经变成 id <> 5，抽取逻辑只需要处理简单形态。
  ConditionPtr where = rewrite_condition_tree(query_where(query));

  if (!schema->has_primary_key()) {
    // 没有主键：只能全表扫描，不算错误
    result.remaining = std::move(where);
    return result;
  }

  const sql::ColumnDef *pk_column = schema->primary_key_column();
  result.pk_column = schema->primary_key_name();
  result.pk_type =
      pk_column != nullptr ? pk_column->type : sql::DataType::UNKNOWN_TYPE;

  if (where) {
    SplitConditions split = split_primary_key(*where, result.pk_column);
    result.pk_cond = std::move(split.pk_cond);
    result.remaining = std::move(split.remaining);
  }
  return result;
}

// ============================================================
// 2) 再重写两棵树
// ============================================================
std::expected<Optimizer::PrimaryKeyExtractResult, PlanError>
Optimizer::rewrite_primary_key_condition(
    const PrimaryKeyExtractResult &pk_result) {
  PrimaryKeyExtractResult result;
  result.db = pk_result.db;
  result.table = pk_result.table;
  result.pk_column = pk_result.pk_column;
  result.pk_type = pk_result.pk_type;
  result.pk_cond = rewrite_condition_tree(pk_result.pk_cond.get());
  result.remaining = rewrite_condition_tree(pk_result.remaining.get());
  return result;
}

// ============================================================
// 3) 主键条件 -> KeyRange / KeySet
// ============================================================
std::expected<OptimizedQuery, PlanError>
Optimizer::convert_to_key_range(const sql::Query &query,
                                const PrimaryKeyExtractResult &pk_result) {
  OptimizedQuery out(clone_query(query));
  out.db = pk_result.db;
  out.table = pk_result.table;
  out.primary_key = pk_result.pk_column;
  out.primary_key_type = pk_result.pk_type;

  std::vector<KeyRange> candidates;
  if (pk_result.pk_cond) {
    auto converted = range_of_condition(*pk_result.pk_cond, pk_result.pk_type);
    if (converted.has_value()) {
      candidates = normalize_ranges(std::move(*converted));
      candidates = coalesce_adjacent_points(std::move(candidates),
                                            pk_result.pk_type);
      if (candidates.empty()) {
        candidates.push_back(KeyRange::empty(pk_result.pk_type));
      }
      // 主键条件完全下推：剩下的非主键条件仍然是执行器必须求值的过滤
      out.remaining_filter =
          pk_result.remaining ? pk_result.remaining->clone() : nullptr;
    } else {
      // 翻译不出来：整棵 pk_cond 退回过滤，候选 = 全表
      ConditionPtr merged = rebuild_split(
          pk_result.pk_cond->clone(),
          pk_result.remaining ? pk_result.remaining->clone() : nullptr);
      out.remaining_filter = rewrite_condition_tree(merged.get());
    }
  } else {
    out.remaining_filter =
        pk_result.remaining ? pk_result.remaining->clone() : nullptr;
  }

  if (candidates.empty()) {
    // 没有主键条件（或翻译失败）：全表扫描
    candidates.push_back(KeyRange::all(pk_result.pk_type));
  }

  // 只有一个概念：有序、互不相交的区间（点就是退化区间）
  out.ranges = std::move(candidates);

  // 排除点只是扫描提示：<> / NOT IN 仍然留在 remaining_filter 里
  collect_exclusions(out.remaining_filter.get(), out.primary_key,
                     out.exclude_keys);

  out.is_point_query = out.ranges.size() == 1 && out.ranges[0].is_point();
  return out;
}

} // namespace plan
