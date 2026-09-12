// planner.cpp
//
// Planner：把 OptimizedQuery 搭成计划树（规则驱动，没有代价模型）。
//
// 核心是**一条公共的扫描链**，SELECT / UPDATE / DELETE 都用它，
// 语句自己的部分只负责往上面套头（Project/Limit/Sort 或 Update/Delete）：
//
//     build_scan_chain(oq, ascending) =
//         Filter? -> RangeUnion? -> IndexScan/FullScan
//
//   单区间且没有要跳过的点 -> IndexScan（点查询就是退化区间）
//   整表且没有要跳过的点   -> FullScan
//   其余（多区间 / 有点要跳过）-> RangeUnion（自己维护多区间拼接的状态机）
//
// 方向与 Sort 消除：
//   ORDER BY 的首列是主键时，扫描顺序就是要求的顺序（主键唯一，后面的
//   排序列永远分不出胜负），只需要订正扫描方向、**不建 Sort 节点**。
//
// UPDATE/DELETE：先搭同一条扫描链，再用 Update/Delete 包一层 ——
//   "扫哪片 key 空间 + 怎么过滤"只表达一次，执行器的写算子只做
//   "拉一行 -> 改/删一行"。没有 WHERE 时链就是 FullScan，
//   绝不会退化成"扫零行"。
#include "planner.h"

#include <cstddef>
#include <limits>
#include <memory>
#include <utility>
#include <vector>

#include "sql_types/key_range.h"

namespace plan {
namespace {

// TopN 需要保留的行数 = OFFSET + LIMIT（saturating，避免 size_t 溢出）
size_t top_n_count(const sql::LimitClause &limit) {
  if (!limit.has_limit()) {
    return 0;
  }
  const size_t want = limit.limit_value();
  const size_t skip = limit.has_offset() ? limit.offset_value() : size_t{0};
  const size_t max = std::numeric_limits<size_t>::max();
  return skip > max - want ? max : want + skip;
}

TableRef make_table_ref(const OptimizedQuery &query) {
  TableRef target;
  target.db = query.db;
  target.table = query.table;
  target.primary_key = query.primary_key;
  target.primary_key_type = query.primary_key_type;
  return target;
}

// ============================================================
// 公共扫描链：Filter? -> RangeUnion? -> IndexScan/FullScan
//
// 注意：扫描空间与过滤条件会被**移出** query（建树时"搬家"，
// 保证树里只有一份真相），移动之后不能再读 query 的这几个字段 ——
// 但仍然可以读语句本身（写语句要把 OptimizedQuery 移进自己的节点）。
// ============================================================
std::unique_ptr<PlanNode> build_scan_chain(OptimizedQuery &query,
                                           bool ascending) {
  const TableRef target = make_table_ref(query);

  sql::ConditionPtr filter = std::move(query.remaining_filter);
  query.remaining_filter = nullptr;
  std::vector<sql::KeyRange> ranges = std::move(query.ranges);
  query.ranges.clear();
  sql::KeySet exclude_keys = std::move(query.exclude_keys);
  query.exclude_keys = sql::KeySet();

  if (ranges.empty()) {
    // 理论上 optimizer 保证 ranges 非空；空的话按"扫不到任何行"处理更安全
    ranges.push_back(sql::KeyRange::empty(target.primary_key_type));
  }

  std::unique_ptr<PlanNode> node;
  const bool has_exclusions = !exclude_keys.empty();
  if (!has_exclusions && ranges.size() == 1 && ranges[0].is_all()) {
    node = std::make_unique<FullScan>(target, ascending);
  } else if (!has_exclusions && ranges.size() == 1) {
    node = std::make_unique<IndexScanPlan>(target, std::move(ranges[0]),
                                           ascending);
  } else {
    node = std::make_unique<RangeUnionPlan>(target, std::move(ranges),
                                            ascending, std::move(exclude_keys));
  }

  if (filter) {
    node = std::make_unique<FilterPlan>(std::move(node), std::move(filter));
  }
  return node;
}

// ORDER BY 首列是主键 -> 扫描顺序已经够用；返回需要的扫描方向
struct ScanOrder {
  bool ascending = true;
  bool satisfied_by_scan = false;
};

ScanOrder order_by_scan(const sql::SelectQuery &select,
                        const OptimizedQuery &query) {
  ScanOrder order;
  if (select.order_by.empty() || query.primary_key.empty()) {
    return order;
  }
  if (select.order_by.front().column != query.primary_key) {
    return order;
  }
  order.satisfied_by_scan = true;
  order.ascending =
      select.order_by.front().direction == sql::OrderDirection::ASC;
  return order;
}

// ============================================================
// SELECT: Project? -> Limit? -> Sort/TopN? -> (扫描链)
// ============================================================
std::expected<std::unique_ptr<PlanNode>, PlanError>
plan_select(OptimizedQuery query) {
  const sql::SelectQuery *select = query.query.select();
  if (select == nullptr) {
    return std::unexpected(
        PlanError(PlanErrorCode::UNSUPPORTED_PLAN, "not a select query"));
  }

  // 先取出要用的信息：build_scan_chain 之后不能再读扫描空间，
  // 而且链里不持有语句（Query 是 move-only）
  const std::vector<sql::OrderByItem> order_by = select->order_by;
  const std::vector<sql::ColumnRef> columns = select->columns;
  const sql::LimitClause limit = select->limit;
  const bool select_all = select->select_all();
  const ScanOrder order = order_by_scan(*select, query);

  std::unique_ptr<PlanNode> node = build_scan_chain(query, order.ascending);

  // Sort / TopN：只有"扫描顺序满足不了 ORDER BY"时才需要
  if (!order_by.empty() && !order.satisfied_by_scan) {
    const bool top_n = limit.has_limit();
    node = std::make_unique<SortPlan>(std::move(node), order_by, top_n,
                                      top_n ? top_n_count(limit) : 0);
  }

  if (limit.has_limit() || limit.has_offset()) {
    node = std::make_unique<LimitPlan>(std::move(node), limit);
  }

  if (!select_all) {
    node = std::make_unique<ProjectPlan>(std::move(node), columns);
  }
  return node;
}

// ============================================================
// UPDATE / DELETE: 写节点 -> (与 SELECT 相同的扫描链)
//
// 先建链、再 move query：函数实参求值顺序不确定，
// 把 build_scan_chain 写在 const std::move(query) 同一个表达式里
// 会读到 moved-from 的语句（这个坑踩过一次）。
// ============================================================
std::expected<std::unique_ptr<PlanNode>, PlanError>
plan_update(OptimizedQuery query) {
  if (query.query.update() == nullptr) {
    return std::unexpected(
        PlanError(PlanErrorCode::UNSUPPORTED_PLAN, "not an update query"));
  }
  std::unique_ptr<PlanNode> chain = build_scan_chain(query, /*ascending=*/true);
  return std::unique_ptr<PlanNode>(
      std::make_unique<UpdatePlan>(std::move(query), std::move(chain)));
}

std::expected<std::unique_ptr<PlanNode>, PlanError>
plan_delete(OptimizedQuery query) {
  if (query.query.delete_() == nullptr) {
    return std::unexpected(
        PlanError(PlanErrorCode::UNSUPPORTED_PLAN, "not a delete query"));
  }
  std::unique_ptr<PlanNode> chain = build_scan_chain(query, /*ascending=*/true);
  return std::unique_ptr<PlanNode>(
      std::make_unique<DeletePlan>(std::move(query), std::move(chain)));
}

} // namespace

std::expected<std::unique_ptr<PlanNode>, PlanError>
Planner::plan(OptimizedQuery query) {
  switch (query.query.type()) {
  case sql::QueryType::SELECT:
    return plan_select(std::move(query));
  case sql::QueryType::UPDATE:
    return plan_update(std::move(query));
  case sql::QueryType::DELETE:
    return plan_delete(std::move(query));
  case sql::QueryType::INSERT:
    // VALUES 作为行源留在语句里，所以 child 为空（为 INSERT ... SELECT 留位）
    return std::unique_ptr<PlanNode>(
        std::make_unique<InsertPlan>(std::move(query), nullptr));
  case sql::QueryType::CREATE_DATABASE:
    return std::unique_ptr<PlanNode>(
        std::make_unique<CreateDatabasePlan>(std::move(query)));
  case sql::QueryType::DROP_DATABASE:
    return std::unique_ptr<PlanNode>(
        std::make_unique<DropDatabasePlan>(std::move(query)));
  case sql::QueryType::CREATE_TABLE:
    return std::unique_ptr<PlanNode>(
        std::make_unique<CreateTablePlan>(std::move(query)));
  case sql::QueryType::DROP_TABLE:
    return std::unique_ptr<PlanNode>(
        std::make_unique<DropTablePlan>(std::move(query)));
  case sql::QueryType::USE_DATABASE:
    return std::unique_ptr<PlanNode>(
        std::make_unique<UseDatabasePlan>(std::move(query)));
  default:
    return std::unexpected(
        PlanError(PlanErrorCode::UNSUPPORTED_PLAN,
                  "cannot plan query: " + query.query.to_string()));
  }
}

} // namespace plan
