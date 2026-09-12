// optimizer.h
#ifndef QUERY_OPTIMIZER_H
#define QUERY_OPTIMIZER_H

#include <memory>

#include "relation/database_manager.h"
#include "plan.h"
#include "query/statement/statement.h"

namespace query {

// ============================================================
// 优化器
// ============================================================
class Optimizer {
 public:
  explicit Optimizer(std::shared_ptr<sql::DatabaseManager> db_manager);
  ~Optimizer() = default;

  // Statement → ExecutionPlan
  std::unique_ptr<ExecutionPlan> optimize(Statement* stmt,
                                          const std::string& db_name);

 private:
  // ----- 具体优化方法 -----
  std::unique_ptr<ExecutionPlan> optimize_use(UseStatement* stmt,
                                              const std::string& db_name);
  std::unique_ptr<ExecutionPlan> optimize_select(SelectStatement* stmt,
                                                 const std::string& db_name);
  std::unique_ptr<ExecutionPlan> optimize_insert(InsertStatement* stmt,
                                                 const std::string& db_name);
  std::unique_ptr<ExecutionPlan> optimize_update(UpdateStatement* stmt,
                                                 const std::string& db_name);
  std::unique_ptr<ExecutionPlan> optimize_delete(DeleteStatement* stmt,
                                                 const std::string& db_name);

  // ----- 优化辅助 -----
  // 尝试将条件下推到主键扫描
  bool try_convert_to_index_scan(
      SelectStatement* stmt, const sql::TableSchema& schema,
      std::unique_ptr<IndexScanPlan>& index_plan,
      std::unique_ptr<ConditionExpr>& remaining_condition);

  // 提取主键范围
  bool extract_pk_range(
      const std::vector<std::pair<CompareOp, sql::Value>>& pk_conds,
      sql::Value& start, sql::Value& end, bool& is_point);

  std::shared_ptr<sql::DatabaseManager> db_manager_;
};

}  // namespace query

#endif  // QUERY_OPTIMIZER_H