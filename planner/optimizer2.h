// optimizer.h
#ifndef OPTIMIZER_H
#define OPTIMIZER_H

#include <memory>

#include "condition.h"
#include "plan.h"
#include "statement.h"
#include "storage_engine.h"

namespace sql {

class Optimizer {
 public:
  explicit Optimizer(std::shared_ptr<storage::StorageEngine> engine);
  ~Optimizer() = default;

  // 优化：Statement → ExecutionPlan
  std::unique_ptr<ExecutionPlan> optimize(Statement* stmt);

 private:
  // 具体优化方法
  std::unique_ptr<ExecutionPlan> optimize_use(UseStatement* stmt);
  std::unique_ptr<ExecutionPlan> optimize_select(SelectStatement* stmt);
  std::unique_ptr<ExecutionPlan> optimize_insert(InsertStatement* stmt);
  std::unique_ptr<ExecutionPlan> optimize_update(UpdateStatement* stmt);
  std::unique_ptr<ExecutionPlan> optimize_delete(DeleteStatement* stmt);

  // 条件 → Key 范围转换（在优化器中做）
  bool convert_to_key_range(const ConditionExpr* cond,
                            const std::string& table_name,
                            storage::KeyRange& key_range,
                            storage::KeySet& key_set);

  // 提取主键范围
  bool extract_pk_range(
      const std::vector<std::pair<CompareOp, storage::Value>>& pk_conds,
      storage::KeyRange& key_range, storage::KeySet& key_set);

  // 生成 Key 前缀
  std::string make_key_prefix(const std::string& table_name) const;
  std::string value_to_key_string(const storage::Value& value) const;

  std::shared_ptr<storage::StorageEngine> engine_;
};

}  // namespace sql

#endif  // OPTIMIZER_H