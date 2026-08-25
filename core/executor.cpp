// executor.cpp
#include "executor.h"

#include <iomanip>
#include <iostream>
#include <sstream>

#include "condition.h"

namespace sql {

// ============================================================
// 构造
// ============================================================
Executor::Executor(std::shared_ptr<storage::StorageEngine> engine)
    : engine_(engine) {}

// ============================================================
// 主执行入口
// ============================================================
ExecResult Executor::execute(ExecutionPlan* plan) {
  if (!plan || !plan->root) {
    return ExecResult(false, "Empty execution plan");
  }

  std::cout << "\n⚡ 执行阶段..." << std::endl;

  switch (plan->stmt_type) {
    case StatementType::USE:
      return execute_use(plan);
    case StatementType::SELECT:
      return execute_select(plan->root.get());
    case StatementType::INSERT:
      return execute_insert(static_cast<InsertPlan*>(plan->root.get()));
    case StatementType::UPDATE:
      return execute_update(static_cast<UpdatePlan*>(plan->root.get()));
    case StatementType::DELETE:
      return execute_delete(static_cast<DeletePlan*>(plan->root.get()));
    default:
      return ExecResult(false, "Unknown statement type");
  }
}

// ============================================================
// USE 语句
// ============================================================
ExecResult Executor::execute_use(ExecutionPlan* plan) {
  // USE 已在 StatementBuilder 中处理
  return ExecResult(true, "Database set");
}

// ============================================================
// SELECT 语句（分发到具体扫描方式）
// ============================================================
ExecResult Executor::execute_select(PlanNode* node) {
  if (node->type() == PlanNodeType::INDEX_SCAN) {
    return execute_index_scan(static_cast<IndexScanPlan*>(node));
  } else if (node->type() == PlanNodeType::SEQUENTIAL_SCAN) {
    return execute_seq_scan(static_cast<SequentialScanPlan*>(node));
  }
  return ExecResult(false, "Unknown select plan type");
}

// ============================================================
// 索引扫描执行
// ============================================================
ExecResult Executor::execute_index_scan(IndexScanPlan* plan) {
  std::cout << "  📋 INDEX SCAN " << plan->table_name << std::endl;

  // 1. 获取表结构
  auto schema = engine_->get_table_schema(plan->table_name);
  if (schema.table_name.empty()) {
    return ExecResult(false, "Table '" + plan->table_name + "' not found");
  }

  // 2. 获取数据
  std::vector<storage::Row> rows;

  if (plan->is_point_query) {
    // 点查询
    rows = engine_->get_by_keys(plan->table_name, plan->key_set);
    std::cout << "  🔍 点查询: " << plan->key_set.size() << " 个 key"
              << std::endl;

    // 打印 key 列表（调试）
    if (plan->key_set.size() <= 5) {
      for (const auto& key : plan->key_set) {
        std::cout << "     - " << key << std::endl;
      }
    }
  } else {
    // 范围查询
    rows = engine_->get_by_key_range(plan->table_name, plan->key_range);
    std::cout << "  🔍 范围查询: [";
    if (plan->key_range.has_start) {
      std::cout << plan->key_range.start;
    } else {
      std::cout << "-inf";
    }
    std::cout << ", ";
    if (plan->key_range.has_end) {
      std::cout << plan->key_range.end;
    } else {
      std::cout << "+inf";
    }
    std::cout << ")" << std::endl;
  }

  std::cout << "  📊 扫描到 " << rows.size() << " 行" << std::endl;

  // 3. 应用额外的过滤条件（非主键条件）
  if (plan->filter_condition) {
    rows = filter_rows(rows, schema, plan->filter_condition.get());
    std::cout << "  🔍 应用过滤条件: " << plan->filter_condition->to_string()
              << std::endl;
    std::cout << "  📊 过滤后 " << rows.size() << " 行" << std::endl;
  }

  // 4. 提取列
  if (!plan->columns.empty() &&
      !(plan->columns.size() == 1 && plan->columns[0] == "*")) {
    rows = extract_columns(rows, schema, plan->columns);
  }

  // 5. 打印结果
  print_result(rows, schema, plan->columns);

  ExecResult result(true, "Query executed");
  result.rows = rows;
  result.affected_rows = static_cast<int64_t>(rows.size());
  return result;
}

// ============================================================
// 顺序扫描执行
// ============================================================
ExecResult Executor::execute_seq_scan(SequentialScanPlan* plan) {
  std::cout << "  📋 SEQUENTIAL SCAN " << plan->table_name << std::endl;

  // 1. 获取表结构
  auto schema = engine_->get_table_schema(plan->table_name);
  if (schema.table_name.empty()) {
    return ExecResult(false, "Table '" + plan->table_name + "' not found");
  }

  // 2. 全表扫描
  std::vector<storage::Row> rows = engine_->scan_all(plan->table_name);
  std::cout << "  🔍 全表扫描: " << rows.size() << " 行" << std::endl;

  // 3. 应用过滤条件
  if (plan->filter_condition) {
    rows = filter_rows(rows, schema, plan->filter_condition.get());
    std::cout << "  🔍 应用过滤条件: " << plan->filter_condition->to_string()
              << std::endl;
    std::cout << "  📊 过滤后 " << rows.size() << " 行" << std::endl;
  }

  // 4. 提取列
  if (!plan->columns.empty() &&
      !(plan->columns.size() == 1 && plan->columns[0] == "*")) {
    rows = extract_columns(rows, schema, plan->columns);
  }

  // 5. 打印结果
  print_result(rows, schema, plan->columns);

  ExecResult result(true, "Query executed");
  result.rows = rows;
  result.affected_rows = static_cast<int64_t>(rows.size());
  return result;
}

// ============================================================
// INSERT 语句执行
// ============================================================
ExecResult Executor::execute_insert(InsertPlan* plan) {
  std::cout << "  📝 INSERT INTO " << plan->table_name << std::endl;

  // 1. 获取表结构
  auto schema = engine_->get_table_schema(plan->table_name);
  if (schema.table_name.empty()) {
    return ExecResult(false, "Table '" + plan->table_name + "' not found");
  }

  // 2. 构建完整行
  storage::Row full_row;
  if (plan->columns.empty()) {
    // 没有指定列，按顺序插入
    full_row = plan->values;
  } else {
    // 按列名匹配
    full_row = build_full_row(plan->values, schema, plan->columns);
  }

  // 3. 如果没有指定主键，自动生成
  if (schema.primary_key_index >= 0) {
    int pk_idx = schema.primary_key_index;
    if (pk_idx >= static_cast<int>(full_row.size()) ||
        full_row[pk_idx].type == storage::DataType::NULL_TYPE) {
      // 自动生成主键（使用时间戳 + 随机数）
      auto now = std::chrono::system_clock::now();
      auto timestamp = std::chrono::duration_cast<std::chrono::microseconds>(
                           now.time_since_epoch())
                           .count();
      std::random_device rd;
      std::mt19937 gen(rd());
      std::uniform_int_distribution<> dis(0, 9999);

      std::ostringstream oss;
      oss << std::hex << std::setfill('0') << std::setw(16) << timestamp
          << std::setw(4) << dis(gen);
      std::string pk = oss.str();

      while (static_cast<int>(full_row.size()) <= pk_idx) {
        full_row.push_back(storage::Value());
      }
      full_row[pk_idx] = storage::Value(pk);
    }
  }

  // 4. 执行插入
  bool success = engine_->insert(plan->table_name, full_row);

  if (success) {
    std::cout << "  ✅ 插入 1 行" << std::endl;
    return ExecResult(true, "Insert successful");
  } else {
    return ExecResult(false, "Insert failed");
  }
}

// ============================================================
// UPDATE 语句执行
// ============================================================
ExecResult Executor::execute_update(UpdatePlan* plan) {
  std::cout << "  📝 UPDATE " << plan->table_name << std::endl;

  // 1. 获取表结构
  auto schema = engine_->get_table_schema(plan->table_name);
  if (schema.table_name.empty()) {
    return ExecResult(false, "Table '" + plan->table_name + "' not found");
  }

  // 2. 先查询需要更新的行
  std::vector<storage::Row> rows;
  if (plan->condition) {
    // 有条件，先过滤
    rows = engine_->scan_all(plan->table_name);
    rows = filter_rows(rows, schema, plan->condition.get());
  } else {
    // 无条件，更新所有行
    rows = engine_->scan_all(plan->table_name);
    std::cout << "  ⚠️  没有 WHERE 条件，更新所有 " << rows.size() << " 行"
              << std::endl;
  }

  if (rows.empty()) {
    std::cout << "  ⚠️  没有匹配的行" << std::endl;
    return ExecResult(true, "No rows to update");
  }

  // 3. 更新每一行
  int64_t updated_count = 0;
  for (const auto& row : rows) {
    // 提取主键
    std::string pk = extract_primary_key(row, schema);
    if (pk.empty()) {
      // 如果没有主键，尝试用所有列构建 key（不推荐）
      continue;
    }

    // 执行更新
    if (engine_->update_by_key(plan->table_name, pk, plan->assignments)) {
      updated_count++;
    }
  }

  std::cout << "  ✅ 更新 " << updated_count << " 行" << std::endl;
  return ExecResult(true, "Update successful");
}

// ============================================================
// DELETE 语句执行
// ============================================================
ExecResult Executor::execute_delete(DeletePlan* plan) {
  std::cout << "  📝 DELETE FROM " << plan->table_name << std::endl;

  // 1. 获取表结构
  auto schema = engine_->get_table_schema(plan->table_name);
  if (schema.table_name.empty()) {
    return ExecResult(false, "Table '" + plan->table_name + "' not found");
  }

  // 2. 先查询需要删除的行
  std::vector<storage::Row> rows;
  if (plan->condition) {
    rows = engine_->scan_all(plan->table_name);
    rows = filter_rows(rows, schema, plan->condition.get());
  } else {
    rows = engine_->scan_all(plan->table_name);
    std::cout << "  ⚠️  没有 WHERE 条件，删除所有 " << rows.size() << " 行"
              << std::endl;
  }

  if (rows.empty()) {
    std::cout << "  ⚠️  没有匹配的行" << std::endl;
    return ExecResult(true, "No rows to delete");
  }

  // 3. 收集主键
  storage::KeySet keys;
  for (const auto& row : rows) {
    std::string pk = extract_primary_key(row, schema);
    if (!pk.empty()) {
      keys.push_back(pk);
    }
  }

  // 4. 批量删除
  bool success = engine_->delete_by_keys(plan->table_name, keys);

  if (success) {
    std::cout << "  ✅ 删除 " << keys.size() << " 行" << std::endl;
    return ExecResult(true, "Delete successful");
  } else {
    return ExecResult(false, "Delete failed");
  }
}

// ============================================================
// 核心数据操作 - 获取行数据
// ============================================================
std::vector<storage::Row> Executor::fetch_rows(
    const std::string& table_name, const std::vector<std::string>& columns,
    const ConditionExpr* condition) {
  std::vector<storage::Row> rows;

  // 获取表结构
  auto schema = engine_->get_table_schema(table_name);
  if (schema.table_name.empty()) {
    return rows;
  }

  // 如果没有条件，直接全表扫描
  if (!condition) {
    return engine_->scan_all(table_name);
  }

  // 尝试优化：提取主键条件
  // 这里简化处理，实际优化在 Optimizer 中已经做了
  // Executor 只执行 Plan，不再做优化
  rows = engine_->scan_all(table_name);

  return rows;
}

// ============================================================
// 应用层过滤
// ============================================================
std::vector<storage::Row> Executor::filter_rows(
    const std::vector<storage::Row>& rows, const storage::TableSchema& schema,
    const ConditionExpr* condition) {
  if (!condition || rows.empty()) {
    return rows;
  }

  std::vector<storage::Row> result;
  result.reserve(rows.size());

  for (const auto& row : rows) {
    if (condition->matches(row, schema)) {
      result.push_back(row);
    }
  }

  return result;
}

// ============================================================
// 提取列
// ============================================================
std::vector<storage::Row> Executor::extract_columns(
    const std::vector<storage::Row>& rows, const storage::TableSchema& schema,
    const std::vector<std::string>& columns) {
  if (columns.empty() || (columns.size() == 1 && columns[0] == "*")) {
    return rows;
  }

  std::vector<storage::Row> result;
  result.reserve(rows.size());

  for (const auto& row : rows) {
    storage::Row extracted;
    extracted.reserve(columns.size());
    for (const auto& col_name : columns) {
      extracted.push_back(get_column_value(row, schema, col_name));
    }
    result.push_back(extracted);
  }

  return result;
}

// ============================================================
// 行操作辅助
// ============================================================

// 获取列值
storage::Value Executor::get_column_value(const storage::Row& row,
                                          const storage::TableSchema& schema,
                                          const std::string& column_name) {
  int idx = schema.get_column_index(column_name);
  if (idx >= 0 && idx < static_cast<int>(row.size())) {
    return row[idx];
  }
  return storage::Value();
}

// 构建完整行（用于 INSERT）
storage::Row Executor::build_full_row(
    const storage::Row& input_row, const storage::TableSchema& schema,
    const std::vector<std::string>& input_columns) {
  storage::Row full_row;
  full_row.reserve(schema.columns.size());

  // 如果列名为空，直接返回输入行
  if (input_columns.empty()) {
    return input_row;
  }

  // 按表结构顺序构建行
  for (const auto& col : schema.columns) {
    bool found = false;
    for (size_t i = 0; i < input_columns.size(); ++i) {
      if (input_columns[i] == col.name) {
        if (i < input_row.size()) {
          full_row.push_back(input_row[i]);
        } else {
          full_row.push_back(storage::Value());
        }
        found = true;
        break;
      }
    }
    if (!found) {
      // 没有提供值，使用 NULL
      full_row.push_back(storage::Value());
    }
  }

  return full_row;
}

// 应用赋值（用于 UPDATE）
storage::Row Executor::apply_assignments(
    const storage::Row& row, const storage::TableSchema& schema,
    const std::vector<std::pair<std::string, storage::Value>>& assignments) {
  storage::Row result = row;

  for (const auto& [col_name, value] : assignments) {
    int idx = schema.get_column_index(col_name);
    if (idx >= 0 && idx < static_cast<int>(result.size())) {
      result[idx] = value;
    }
  }

  return result;
}

// 提取主键值
std::string Executor::extract_primary_key(const storage::Row& row,
                                          const storage::TableSchema& schema) {
  if (schema.primary_key_index < 0) {
    return "";
  }

  int pk_idx = schema.primary_key_index;
  if (pk_idx >= static_cast<int>(row.size())) {
    return "";
  }

  const storage::Value& pk_val = row[pk_idx];
  return pk_val.to_string();
}

// ============================================================
// 结果打印
// ============================================================
void Executor::print_result(const std::vector<storage::Row>& rows,
                            const storage::TableSchema& schema,
                            const std::vector<std::string>& columns) {
  if (rows.empty()) {
    std::cout << "  📋 查询结果: (空)" << std::endl;
    return;
  }

  // 确定要显示的列
  std::vector<std::string> display_cols;
  if (columns.empty() || (columns.size() == 1 && columns[0] == "*")) {
    for (const auto& col : schema.columns) {
      display_cols.push_back(col.name);
    }
  } else {
    display_cols = columns;
  }

  std::cout << "  📋 查询结果 (" << rows.size() << " 行):" << std::endl;

  // 计算每列的最大宽度
  std::vector<size_t> col_widths;
  for (const auto& col_name : display_cols) {
    col_widths.push_back(col_name.length());
  }

  for (const auto& row : rows) {
    for (size_t i = 0; i < row.size() && i < display_cols.size(); ++i) {
      size_t len = row[i].to_string().length();
      if (len > col_widths[i]) {
        col_widths[i] = len;
      }
    }
  }

  // 打印表头
  std::cout << "    ";
  for (size_t i = 0; i < display_cols.size(); ++i) {
    if (i > 0) std::cout << " | ";
    std::cout << std::setw(static_cast<int>(col_widths[i])) << display_cols[i];
  }
  std::cout << std::endl;

  // 打印分隔线
  std::cout << "    ";
  for (size_t i = 0; i < display_cols.size(); ++i) {
    if (i > 0) std::cout << "-+-";
    std::cout << std::string(col_widths[i], '-');
  }
  std::cout << std::endl;

  // 打印数据
  for (const auto& row : rows) {
    std::cout << "    ";
    for (size_t i = 0; i < row.size() && i < display_cols.size(); ++i) {
      if (i > 0) std::cout << " | ";
      std::cout << std::setw(static_cast<int>(col_widths[i]))
                << row[i].to_string();
    }
    std::cout << std::endl;
  }
}

}  // namespace sql