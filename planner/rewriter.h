// rewriter.h
#ifndef REWRITER_H
#define REWRITER_H

#include <expected>

#include "planner_defs.h"
#include "sql_types/condition.h"
#include "sql_types/query.h"

namespace plan {

// 查询重写：简化优化查询
class QueryRewriter {
public:
  // call rewrite_conditions for each statement type of basically dml query
  // return a new query with rewritten conditions
  // ddl/control query will be returned as is
  std::expected<sql::Query, PlanError> rewrite(const sql::Query &query);

private:
  // 条件重写
  sql::ConditionPtr rewrite_conditions(const sql::Condition *cond_root);

  // 常量折叠：IN 列表去重、单元素 IN 退化成比较
  // （区间合并交给 KeyRange / Optimizer：重写器不知道列类型，
  //   也不该再做一套 value±1 的区间算术）
  sql::ConditionPtr fold_constants(const sql::Condition *cond_root);

  // 去重条件
  sql::ConditionPtr deduplicate(const sql::Condition *cond_root);

  // pushdown not
  sql::ConditionPtr pushdown_not(const sql::Condition *cond_root);

  // flattern
  sql::ConditionPtr flattern(const sql::Condition *cond_root);

  // simplify
  sql::ConditionPtr simplify(const sql::Condition *cond_root);
};

// ============================================================
// 条件树重写流水线（optimizer 直接复用，避免两份实现）
//   pushdown_not -> flattern -> fold_constants -> deduplicate -> simplify
// 入参只读，返回新树；root 为 nullptr 时返回 nullptr。
// ============================================================
sql::ConditionPtr rewrite_condition_tree(const sql::Condition *root);

// ============================================================
// Query 工具（rewriter 与 optimizer 共用）
// ============================================================

// Query 是 move-only（内含 ConditionPtr），这里深拷贝一份，条件树用 clone
sql::Query clone_query(const sql::Query &query);

// 取语句的 WHERE 条件；没有 WHERE（或该语句类型没有 WHERE）返回 nullptr
const sql::Condition *query_where(const sql::Query &query);

} // namespace plan

#endif // REWRITER_H
