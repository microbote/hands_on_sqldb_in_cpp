// executor.cpp
#include "executor.h"

#include <chrono>
#include <iomanip>
#include <iostream>
#include <random>
#include <sstream>

namespace sql {

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
      return execute_select(static_cast<SequentialScanPlan*>(plan->root.get()));
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
// SELECT 语句（核心）
// ============================================================
ExecResult Executor::execute_select(SequentialScanPlan* plan) {
  if (!plan || !engine_) {
    return ExecResult(false, "Invalid select plan");
  }

  std::cout << "  📋 SELECT FROM " << plan->table_name << std::endl;

  // 1. 获取表结构
  auto schema = engine_->get_table_schema(plan->table_name);
  if (schema.table_name.empty()) {
    return ExecResult(false, "Table '" + plan->table_name + "' not found");
  }

  // 2. 获取数据（通过 Converter 优化）
  std::vector<storage::Row> rows =
      fetch_rows(plan->table_name, plan->columns, plan->condition.get());

  // 3. 提取需要的列
  if (!plan->columns.empty() &&
      !(plan->columns.size() == 1 && plan->columns[0] == "*")) {
    rows = extract_columns(rows, schema, plan->columns);
  }

  // 4. 打印结果
  print_result(rows, schema, plan->columns);

  ExecResult result(true, "Query executed");
  result.rows = rows;
  result.affected_rows = static_cast<int64_t>(rows.size());
  return result;
}

// ============================================================
// INSERT 语句
// ============================================================
ExecResult Executor::execute_insert(InsertPlan* plan) {
  if (!plan || !engine_) {
    return ExecResult(false, "Invalid insert plan");
  }

  std::cout << "  📝 INSERT INTO " << plan->table_name << std::endl;

  // 1. 获取表结构
  auto schema = engine_->get_table_schema(plan->table_name);
  if (schema.table_name.empty()) {
    return ExecResult(false, "Table '" + plan->table_name + "' not found");
  }

  // 2. 构建完整行
  storage::Row full_row = build_full_row(plan->values, schema, plan->columns);

  // 3. 如果没有指定主键，自动生成
  if (schema.primary_key_index >= 0) {
    int pk_idx = schema.primary_key_index;
    if (pk_idx >= static_cast<int>(full_row.size()) ||
        full_row[pk_idx].type == storage::DataType::NULL_TYPE) {
      // 自动生成主键
      std::string pk = generate_primary_key(schema);
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
// UPDATE 语句
// ============================================================
ExecResult Executor::execute_update(UpdatePlan* plan) {
  if (!plan || !engine_) {
    return ExecResult(false, "Invalid update plan");
  }

  std::cout << "  📝 UPDATE " << plan->table_name << std::endl;

  // 1. 获取表结构
  auto schema = engine_->get_table_schema(plan->table_name);
  if (schema.table_name.empty()) {
    return ExecResult(false, "Table '" + plan->table_name + "' not found");
  }

  // 2. 先查询需要更新的行
  std::vector<storage::Row> rows =
      fetch_rows(plan->table_name, {}, plan->condition.get());

  if (rows.empty()) {
    std::cout << "  ⚠️  没有匹配的行" << std::endl;
    return ExecResult(true, "No rows to update");
  }

  // 3. 更新每一行
  int64_t updated_count = 0;
  for (const auto& row : rows) {
    // 提取主键
    std::string pk = extract_primary_key(row, schema);
    if (pk.empty()) continue;

    // 构建更新后的行
    storage::Row updated_row = row;
    for (const auto& assign : plan->assignments) {
      int idx = schema.get_column_index(assign.first);
      if (idx >= 0 && idx < static_cast<int>(updated_row.size())) {
        updated_row[idx] = assign.second;
      }
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
// DELETE 语句
// ============================================================
ExecResult Executor::execute_delete(DeletePlan* plan) {
  if (!plan || !engine_) {
    return ExecResult(false, "Invalid delete plan");
  }

  std::cout << "  📝 DELETE FROM " << plan->table_name << std::endl;

  // 1. 获取表结构
  auto schema = engine_->get_table_schema(plan->table_name);
  if (schema.table_name.empty()) {
    return ExecResult(false, "Table '" + plan->table_name + "' not found");
  }

  // 2. 先查询需要删除的行
  std::vector<storage::Row> rows =
      fetch_rows(plan->table_name, {}, plan->condition.get());

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
// 核心数据获取方法
// ============================================================
std::vector<storage::Row> Executor::fetch_rows(
    const std::string& table_name, const std::vector<std::string>& columns,
    const ConditionExpr* condition) {
  std::vector<storage::Row> rows;

  // 1. 尝试使用 ConditionConverter 优化
  if (condition) {
    auto schema = engine_->get_table_schema(table_name);
    if (schema.table_name.empty()) {
      return rows;
    }

    ConditionConverter converter(schema);
    storage::KeyRange key_range;
    storage::KeySet key_set;

    bool can_convert = converter.convert(condition, key_range, key_set);

    if (can_convert) {
      // 使用 KV 层优化查询
      if (!key_set.empty()) {
        // 点查询（主键 EQ）
        rows = engine_->get_by_keys(table_name, key_set);
        std::cout << "  🔍 点查询: " << key_set.size() << " 个 key"
                  << std::endl;
      } else if (key_range.has_start || key_range.has_end) {
        // 范围查询（主键范围）
        rows = engine_->scan_by_key_range(table_name, key_range);
        std::cout << "  🔍 范围查询: [" << key_range.start << ", "
                  << key_range.end << ")" << std::endl;
      } else {
        // 回退到全表扫描
        rows = engine_->scan_all(table_name);
        std::cout << "  🔍 全表扫描" << std::endl;
      }
    } else {
      // 无法优化，全表扫描
      rows = engine_->scan_all(table_name);
      std::cout << "  🔍 全表扫描 (无法优化)" << std::endl;
    }

    // 2. 在应用层过滤非主键条件
    if (!rows.empty() && condition) {
      rows = filter_rows(rows, schema, condition);
    }
  } else {
    // 无条件，全表扫描
    rows = engine_->scan_all(table_name);
    std::cout << "  🔍 全表扫描 (无条件)" << std::endl;
  }

  return rows;
}

// ============================================================
// 行过滤（应用层）
// ============================================================
std::vector<storage::Row> Executor::filter_rows(
    const std::vector<storage::Row>& rows, const storage::TableSchema& schema,
    const ConditionExpr* condition) {
  if (!condition) return rows;

  std::vector<storage::Row> result;

  for (const auto& row : rows) {
    if (condition->matches(row, schema)) {
      result.push_back(row);
    }
  }

  return result;
}

// ============================================================
// 列提取
// ============================================================
std::vector<storage::Row> Executor::extract_columns(
    const std::vector<storage::Row>& rows, const storage::TableSchema& schema,
    const std::vector<std::string>& columns) {
  if (columns.empty() || (columns.size() == 1 && columns[0] == "*")) {
    return rows;
  }

  std::vector<storage::Row> result;

  for (const auto& row : rows) {
    storage::Row extracted;
    for (const auto& col_name : columns) {
      extracted.push_back(get_column_value(row, schema, col_name));
    }
    result.push_back(extracted);
  }

  return result;
}

// ============================================================
// 辅助方法
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

  // 如果没有指定列，按顺序插入
  if (input_columns.empty()) {
    return input_row;
  }

  // 按表结构顺序构建行
  for (const auto& col : schema.columns) {
    bool found = false;
    for (size_t i = 0; i < input_columns.size(); ++i) {
      if (input_columns[i] == col.name) {
        full_row.push_back(input_row[i]);
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

// 生成主键值（时间戳 + 随机数）
std::string Executor::generate_primary_key(const storage::TableSchema& schema) {
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
  return oss.str();
}

// 将行转换为 KV
std::pair<std::string, std::string> Executor::row_to_kv(
    const storage::Row& row, const storage::TableSchema& schema) {
  // 提取主键
  std::string pk = extract_primary_key(row, schema);
  if (pk.empty()) {
    // 如果没有主键，生成一个
    pk = generate_primary_key(schema);
  }

  // 构建 key: data:table_name:pk
  std::string key = "data:" + schema.table_name + ":" + pk;

  // 构建 value: col1|col2|col3|...
  std::ostringstream oss;
  for (size_t i = 0; i < row.size(); ++i) {
    if (i > 0) oss << "|";
    oss << row[i].to_string();
  }

  return {key, oss.str()};
}

// 将 KV 转换为行
storage::Row Executor::kv_to_row(const std::string& key,
                                 const std::string& value,
                                 const storage::TableSchema& schema) {
  storage::Row row;

  // 解析 value: col1|col2|col3|...
  std::stringstream ss(value);
  std::string val_str;
  size_t col_idx = 0;

  while (std::getline(ss, val_str, '|')) {
    if (col_idx < schema.columns.size()) {
      storage::Value val =
          storage::Value::from_string(val_str, schema.columns[col_idx].type);
      row.push_back(val);
    }
    col_idx++;
  }

  return row;
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
    std::cout << std::setw(col_widths[i]) << display_cols[i];
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
      std::cout << std::setw(col_widths[i]) << row[i].to_string();
    }
    std::cout << std::endl;
  }
}

}  // namespace sql