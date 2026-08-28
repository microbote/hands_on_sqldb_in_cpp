// executor.h
#ifndef QUERY_EXECUTOR_H
#define QUERY_EXECUTOR_H

#include <memory>
#include <vector>

#include "relation/sql_relation.h"
#include "query/planner/plan.h"

namespace query {

// ============================================================
// 执行结果
// ============================================================
struct ExecResult {
  bool success;
  std::string message;
  std::vector<sql::Row> rows;
  int64_t affected_rows;

  ExecResult() : success(false), affected_rows(0) {}
  ExecResult(bool s, const std::string& msg = "")
      : success(s), message(msg), affected_rows(0) {}
};

// ============================================================
// 执行器
// ============================================================
class Executor {
 public:
  explicit Executor(std::shared_ptr<sql::DatabaseManager> db_manager);
  ~Executor() = default;

  // 执行计划
  ExecResult execute(ExecutionPlan* plan);

 private:
  // ----- 具体执行方法 -----
  ExecResult execute_use(ExecutionPlan* plan);
  ExecResult execute_sequential_scan(SequentialScanPlan* plan,
                                     const std::string& db_name);
  ExecResult execute_index_scan(IndexScanPlan* plan,
                                const std::string& db_name);
  ExecResult execute_insert(InsertPlan* plan, const std::string& db_name);
  ExecResult execute_update(UpdatePlan* plan, const std::string& db_name);
  ExecResult execute_delete(DeletePlan* plan, const std::string& db_name);

  // ----- 辅助方法 -----
  std::vector<sql::Row> filter_rows(const std::vector<sql::Row>& rows,
                                    const sql::TableSchema& schema,
                                    const ConditionExpr* condition);

  std::vector<sql::Row> project_columns(
      const std::vector<sql::Row>& rows, const sql::TableSchema& schema,
      const std::vector<std::string>& columns);

  void print_result(const std::vector<sql::Row>& rows,
                    const sql::TableSchema& schema,
                    const std::vector<std::string>& columns);

  std::shared_ptr<sql::DatabaseManager> db_manager_;
  std::string current_db_;
};

}  // namespace query

#endif  // QUERY_EXECUTOR_H