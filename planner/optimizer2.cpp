// optimizer.cpp
#include "optimizer.h"

#include <iomanip>
#include <iostream>
#include <sstream>

namespace sql {

Optimizer::Optimizer(std::shared_ptr<storage::StorageEngine> engine)
    : engine_(engine) {}

std::unique_ptr<ExecutionPlan> Optimizer::optimize(Statement* stmt) {
  if (!stmt || !stmt->is_valid()) {
    return nullptr;
  }

  std::cout << "\n🔧 优化阶段..." << std::endl;

  switch (stmt->type()) {
    case StatementType::USE:
      return optimize_use(static_cast<UseStatement*>(stmt));
    case StatementType::SELECT:
      return optimize_select(static_cast<SelectStatement*>(stmt));
    case StatementType::INSERT:
      return optimize_insert(static_cast<InsertStatement*>(stmt));
    case StatementType::UPDATE:
      return optimize_update(static_cast<UpdateStatement*>(stmt));
    case StatementType::DELETE:
      return optimize_delete(static_cast<DeleteStatement*>(stmt));
    default:
      return nullptr;
  }
}

std::unique_ptr<ExecutionPlan> Optimizer::optimize_use(UseStatement* stmt) {
  return std::make_unique<ExecutionPlan>(StatementType::USE);
}

// ============================================================
// SELECT 优化（核心）
// ============================================================
std::unique_ptr<ExecutionPlan> Optimizer::optimize_select(
    SelectStatement* stmt) {
  auto plan = std::make_unique<ExecutionPlan>(StatementType::SELECT);

  std::cout << "  📊 SELECT FROM " << stmt->table_name << std::endl;
  if (stmt->condition) {
    std::cout << "  📝 条件: " << stmt->condition->to_string() << std::endl;
  }

  // 尝试将条件转换为 KeyRange/KeySet
  storage::KeyRange key_range;
  storage::KeySet key_set;
  bool can_use_index = false;
  std::unique_ptr<ConditionExpr> remaining_filter;

  if (stmt->condition) {
    can_use_index = convert_to_key_range(stmt->condition.get(),
                                         stmt->table_name, key_range, key_set);

    // 如果转换成功，检查是否还有剩余的过滤条件
    if (can_use_index) {
      // 检查原始条件是否完全被主键条件覆盖
      auto schema = engine_->get_table_schema(stmt->table_name);
      std::string pk_column = schema.get_primary_key_column();

      // 检查是否还有非主键条件
      if (!stmt->condition->only_primary_key(pk_column)) {
        // 保留非主键条件作为过滤
        remaining_filter = std::make_unique<ConditionExpr>(*stmt->condition);
        // 注意：这里简单处理，实际需要分离主键和非主键条件
        std::cout << "  ℹ️  保留非主键条件作为过滤" << std::endl;
      }
    }
  }

  if (can_use_index) {
    // 使用索引扫描
    auto index_plan = std::make_unique<IndexScanPlan>(stmt->table_name);
    index_plan->columns = stmt->columns;
    index_plan->key_range = key_range;
    index_plan->key_set = key_set;
    index_plan->is_point_query = !key_set.empty();
    if (remaining_filter) {
      index_plan->filter_condition = std::move(remaining_filter);
    }
    plan->root = std::move(index_plan);
    std::cout << "  ✅ 使用索引扫描" << std::endl;
  } else {
    // 全表扫描
    auto seq_plan = std::make_unique<SequentialScanPlan>(stmt->table_name);
    seq_plan->columns = stmt->columns;
    if (stmt->condition) {
      seq_plan->filter_condition =
          std::make_unique<ConditionExpr>(*stmt->condition);
    }
    plan->root = std::move(seq_plan);
    std::cout << "  ⚠️  使用全表扫描" << std::endl;
  }

  return plan;
}

// ============================================================
// 条件 → Key 范围转换
// ============================================================
bool Optimizer::convert_to_key_range(const ConditionExpr* cond,
                                     const std::string& table_name,
                                     storage::KeyRange& key_range,
                                     storage::KeySet& key_set) {
  if (!cond) return false;

  auto schema = engine_->get_table_schema(table_name);
  if (schema.table_name.empty()) {
    return false;
  }

  std::string pk_column = schema.get_primary_key_column();
  if (pk_column.empty()) {
    return false;
  }

  // 提取主键条件
  std::vector<std::pair<CompareOp, storage::Value>> pk_conds;
  if (!cond->extract_primary_key_conditions(pk_column, pk_conds)) {
    return false;
  }

  return extract_pk_range(pk_conds, key_range, key_set);
}

bool Optimizer::extract_pk_range(
    const std::vector<std::pair<CompareOp, storage::Value>>& pk_conds,
    storage::KeyRange& key_range, storage::KeySet& key_set) {
  if (pk_conds.empty()) return false;

  std::string prefix = make_key_prefix("");
  bool has_eq = false;
  storage::Value eq_value;
  storage::Value gt_value, ge_value, lt_value, le_value;
  bool has_gt = false, has_ge = false, has_lt = false, has_le = false;

  // 分析所有条件
  for (const auto& [op, val] : pk_conds) {
    switch (op) {
      case CompareOp::EQ:
        has_eq = true;
        eq_value = val;
        break;
      case CompareOp::GT:
        has_gt = true;
        gt_value = val;
        break;
      case CompareOp::GE:
        has_ge = true;
        ge_value = val;
        break;
      case CompareOp::LT:
        has_lt = true;
        lt_value = val;
        break;
      case CompareOp::LE:
        has_le = true;
        le_value = val;
        break;
      default:
        break;
    }
  }

  // 1. 如果有 EQ，直接点查询
  if (has_eq) {
    // 检查是否有矛盾的范围条件
    if ((has_gt && !(eq_value > gt_value)) ||
        (has_ge && !(eq_value >= ge_value)) ||
        (has_lt && !(eq_value < lt_value)) ||
        (has_le && !(eq_value <= le_value))) {
      return false;  // 矛盾条件，无结果
    }

    key_set.push_back(value_to_key_string(eq_value));
    return true;
  }

  // 2. 范围查询
  key_range.has_start = false;
  key_range.has_end = false;

  // 处理下限
  if (has_gt) {
    key_range.start = value_to_key_string(gt_value) + "\x00";
    key_range.has_start = true;
  } else if (has_ge) {
    key_range.start = value_to_key_string(ge_value);
    key_range.has_start = true;
  }

  // 处理上限
  if (has_lt) {
    key_range.end = value_to_key_string(lt_value);
    key_range.has_end = true;
  } else if (has_le) {
    key_range.end = value_to_key_string(le_value) + "\x00";
    key_range.has_end = true;
  }

  // 检查是否有效
  if (key_range.has_start && key_range.has_end) {
    if (key_range.start >= key_range.end) {
      return false;  // 空范围
    }
  }

  return key_range.has_start || key_range.has_end;
}

// ============================================================
// 辅助方法
// ============================================================
std::string Optimizer::make_key_prefix(const std::string& table_name) const {
  return "data:" + table_name + ":";
}

std::string Optimizer::value_to_key_string(const storage::Value& value) const {
  switch (value.type) {
    case storage::DataType::INTEGER:
      std::ostringstream oss;
      oss << std::setfill('0') << std::setw(20) << value.int_val;
      return oss.str();
    case storage::DataType::STRING:
      return value.str_val;
    case storage::DataType::BOOLEAN:
      return value.bool_val ? "true" : "false";
    default:
      return "";
  }
}

// ============================================================
// 其他优化
// ============================================================
std::unique_ptr<ExecutionPlan> Optimizer::optimize_insert(
    InsertStatement* stmt) {
  auto plan = std::make_unique<ExecutionPlan>(StatementType::INSERT);
  auto insert_plan = std::make_unique<InsertPlan>(stmt->table_name);
  insert_plan->columns = stmt->columns;
  insert_plan->values = stmt->values;
  plan->root = std::move(insert_plan);
  return plan;
}

std::unique_ptr<ExecutionPlan> Optimizer::optimize_update(
    UpdateStatement* stmt) {
  auto plan = std::make_unique<ExecutionPlan>(StatementType::UPDATE);
  auto update_plan = std::make_unique<UpdatePlan>(stmt->table_name);
  update_plan->assignments = stmt->assignments;
  if (stmt->condition) {
    update_plan->condition = std::make_unique<ConditionExpr>(*stmt->condition);
  }
  plan->root = std::move(update_plan);
  return plan;
}

std::unique_ptr<ExecutionPlan> Optimizer::optimize_delete(
    DeleteStatement* stmt) {
  auto plan = std::make_unique<ExecutionPlan>(StatementType::DELETE);
  auto delete_plan = std::make_unique<DeletePlan>(stmt->table_name);
  if (stmt->condition) {
    delete_plan->condition = std::make_unique<ConditionExpr>(*stmt->condition);
  }
  plan->root = std::move(delete_plan);
  return plan;
}

}  // namespace sql