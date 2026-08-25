// executor.h
#ifndef EXECUTOR_H
#define EXECUTOR_H

#include <memory>
#include <string>
#include <vector>

#include "condition.h"
#include "plan.h"
#include "storage_engine.h"

namespace sql {

// ============ 执行结果 ============
struct ExecResult {
  bool success;
  std::string message;
  std::vector<storage::Row> rows;
  int64_t affected_rows;

  ExecResult() : success(false), affected_rows(0) {}
  ExecResult(bool s, const std::string& msg = "")
      : success(s), message(msg), affected_rows(0) {}
};

// ============ 执行器 ============
class Executor {
 public:
  explicit Executor(std::shared_ptr<storage::StorageEngine> engine);
  ~Executor() = default;

  // 执行计划
  ExecResult execute(ExecutionPlan* plan);

 private:
  // ============================================================
  // 具体执行方法
  // ============================================================
  ExecResult execute_use(ExecutionPlan* plan);
  ExecResult execute_select(PlanNode* node);
  ExecResult execute_insert(InsertPlan* plan);
  ExecResult execute_update(UpdatePlan* plan);
  ExecResult execute_delete(DeletePlan* plan);

  // ============================================================
  // 扫描执行
  // ============================================================
  ExecResult execute_index_scan(IndexScanPlan* plan);
  ExecResult execute_seq_scan(SequentialScanPlan* plan);

  // ============================================================
  // 核心数据操作
  // ============================================================
  // 获取行数据（根据计划类型自动选择最优方式）
  std::vector<storage::Row> fetch_rows(const std::string& table_name,
                                       const std::vector<std::string>& columns,
                                       const ConditionExpr* condition);

  // 应用层过滤（用于非索引条件）
  std::vector<storage::Row> filter_rows(const std::vector<storage::Row>& rows,
                                        const storage::TableSchema& schema,
                                        const ConditionExpr* condition);

  // 提取指定列
  std::vector<storage::Row> extract_columns(
      const std::vector<storage::Row>& rows, const storage::TableSchema& schema,
      const std::vector<std::string>& columns);

  // ============================================================
  // 行操作辅助
  // ============================================================
  storage::Value get_column_value(const storage::Row& row,
                                  const storage::TableSchema& schema,
                                  const std::string& column_name);

  storage::Row build_full_row(const storage::Row& input_row,
                              const storage::TableSchema& schema,
                              const std::vector<std::string>& input_columns);

  storage::Row apply_assignments(
      const storage::Row& row, const storage::TableSchema& schema,
      const std::vector<std::pair<std::string, storage::Value>>& assignments);

  std::string extract_primary_key(const storage::Row& row,
                                  const storage::TableSchema& schema);

  // ============================================================
  // 结果打印
  // ============================================================
  void print_result(const std::vector<storage::Row>& rows,
                    const storage::TableSchema& schema,
                    const std::vector<std::string>& columns);

  // ============================================================
  // 成员变量
  // ============================================================
  std::shared_ptr<storage::StorageEngine> engine_;
};

}  // namespace sql

#endif  // EXECUTOR_H