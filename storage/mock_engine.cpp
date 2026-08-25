// mock_engine.cpp
#include "mock_engine.h"

#include <algorithm>
#include <chrono>
#include <iomanip>
#include <iostream>
#include <random>
#include <sstream>

namespace storage {

// ============================================================
// Value 辅助方法
// ============================================================
std::string Value::to_string() const {
  switch (type) {
    case DataType::INTEGER:
      return std::to_string(int_val);
    case DataType::STRING:
      return str_val;
    case DataType::BOOLEAN:
      return bool_val ? "true" : "false";
    case DataType::NULL_TYPE:
      return "NULL";
  }
  return "NULL";
}

Value Value::from_string(const std::string& str, DataType type) {
  switch (type) {
    case DataType::INTEGER:
      return Value(std::stoll(str));
    case DataType::STRING:
      return Value(str);
    case DataType::BOOLEAN:
      return Value(str == "true" || str == "1");
    default:
      return Value();
  }
}

// ============================================================
// TableSchema 辅助方法
// ============================================================
int TableSchema::get_column_index(const std::string& name) const {
  for (size_t i = 0; i < columns.size(); ++i) {
    if (columns[i].name == name) {
      return static_cast<int>(i);
    }
  }
  return -1;
}

const ColumnDef* TableSchema::get_column(const std::string& name) const {
  int idx = get_column_index(name);
  if (idx >= 0 && idx < static_cast<int>(columns.size())) {
    return &columns[idx];
  }
  return nullptr;
}

std::string TableSchema::get_primary_key_column() const {
  if (primary_key_index >= 0 &&
      primary_key_index < static_cast<int>(columns.size())) {
    return columns[primary_key_index].name;
  }
  return "";
}

// ============================================================
// KVStore 实现
// ============================================================
bool MockEngine::put(const std::string& key, const std::string& value) {
  std::lock_guard<std::mutex> lock(mutex_);
  // 查找对应的表
  for (auto& [table_name, data] : tables_) {
    if (key.find(encode_key_prefix(table_name)) == 0) {
      data.rows[key] = value;
      return true;
    }
  }
  // 如果没找到表，无法存储
  return false;
}

bool MockEngine::get(const std::string& key, std::string& value) {
  std::lock_guard<std::mutex> lock(mutex_);
  for (const auto& [table_name, data] : tables_) {
    auto it = data.rows.find(key);
    if (it != data.rows.end()) {
      value = it->second;
      return true;
    }
  }
  return false;
}

bool MockEngine::del(const std::string& key) {
  std::lock_guard<std::mutex> lock(mutex_);
  for (auto& [table_name, data] : tables_) {
    auto it = data.rows.find(key);
    if (it != data.rows.end()) {
      data.rows.erase(it);
      return true;
    }
  }
  return false;
}

bool MockEngine::batch_put(
    const std::vector<std::pair<std::string, std::string>>& kv_pairs) {
  std::lock_guard<std::mutex> lock(mutex_);
  for (const auto& [key, value] : kv_pairs) {
    bool found = false;
    for (auto& [table_name, data] : tables_) {
      if (key.find(encode_key_prefix(table_name)) == 0) {
        data.rows[key] = value;
        found = true;
        break;
      }
    }
    if (!found) return false;
  }
  return true;
}

std::vector<std::pair<std::string, std::string>> MockEngine::scan(
    const std::string& start, const std::string& end) {
  std::lock_guard<std::mutex> lock(mutex_);
  std::vector<std::pair<std::string, std::string>> result;

  for (const auto& [table_name, data] : tables_) {
    for (const auto& [key, value] : data.rows) {
      if (key >= start && (end.empty() || key < end)) {
        result.push_back({key, value});
      }
    }
  }

  // 按 key 排序
  std::sort(result.begin(), result.end(),
            [](const auto& a, const auto& b) { return a.first < b.first; });
  return result;
}

std::vector<std::pair<std::string, std::string>> MockEngine::scan_prefix(
    const std::string& prefix) {
  std::lock_guard<std::mutex> lock(mutex_);
  std::vector<std::pair<std::string, std::string>> result;

  for (const auto& [table_name, data] : tables_) {
    for (const auto& [key, value] : data.rows) {
      if (key.find(prefix) == 0) {
        result.push_back({key, value});
      }
    }
  }

  std::sort(result.begin(), result.end(),
            [](const auto& a, const auto& b) { return a.first < b.first; });
  return result;
}

void MockEngine::flush() {
  // Mock 引擎不需要 flush
}

void MockEngine::close() {
  // Mock 引擎不需要 close
}

// ============================================================
// TableStore 实现 - 表操作
// ============================================================
bool MockEngine::create_table(const TableSchema& schema) {
  std::lock_guard<std::mutex> lock(mutex_);

  if (table_exists(schema.table_name)) {
    return false;
  }

  TableData data;
  data.schema = schema;
  data.next_id = 1;
  tables_[schema.table_name] = data;

  return true;
}

bool MockEngine::drop_table(const std::string& table_name) {
  std::lock_guard<std::mutex> lock(mutex_);

  auto it = tables_.find(table_name);
  if (it == tables_.end()) {
    return false;
  }

  tables_.erase(it);
  return true;
}

bool MockEngine::table_exists(const std::string& table_name) {
  std::lock_guard<std::mutex> lock(mutex_);
  return tables_.find(table_name) != tables_.end();
}

std::vector<std::string> MockEngine::list_tables() {
  std::lock_guard<std::mutex> lock(mutex_);
  std::vector<std::string> result;
  for (const auto& [name, _] : tables_) {
    result.push_back(name);
  }
  return result;
}

TableSchema MockEngine::get_table_schema(const std::string& table_name) {
  std::lock_guard<std::mutex> lock(mutex_);
  auto it = tables_.find(table_name);
  if (it == tables_.end()) {
    return TableSchema();
  }
  return it->second.schema;
}

// ============================================================
// TableStore 实现 - 插入
// ============================================================
bool MockEngine::insert(const std::string& table_name, const Row& row) {
  std::lock_guard<std::mutex> lock(mutex_);

  auto it = tables_.find(table_name);
  if (it == tables_.end()) {
    return false;
  }

  TableData& data = it->second;
  const TableSchema& schema = data.schema;

  // 构建完整行（自动填充默认值）
  Row full_row = row;

  // 如果主键为空，自动生成
  if (schema.primary_key_index >= 0) {
    int pk_idx = schema.primary_key_index;
    if (pk_idx >= static_cast<int>(full_row.size()) ||
        full_row[pk_idx].type == DataType::NULL_TYPE) {
      std::string pk = generate_next_id(table_name);
      if (pk_idx >= static_cast<int>(full_row.size())) {
        while (static_cast<int>(full_row.size()) <= pk_idx) {
          full_row.push_back(Value());
        }
      }
      full_row[pk_idx] = Value(pk);
    }
  }

  // 编码并存储
  std::string pk = get_primary_key(full_row, schema);
  if (pk.empty()) {
    return false;
  }

  std::string key = encode_key(table_name, pk);
  std::string value = encode_row(full_row, schema);
  data.rows[key] = value;

  return true;
}

bool MockEngine::insert_batch(const std::string& table_name,
                              const std::vector<Row>& rows) {
  bool success = true;
  for (const auto& row : rows) {
    if (!insert(table_name, row)) {
      success = false;
    }
  }
  return success;
}

// ============================================================
// TableStore 实现 - 查询
// ============================================================
bool MockEngine::get_by_key(const std::string& table_name,
                            const std::string& primary_key, Row& row) {
  std::lock_guard<std::mutex> lock(mutex_);

  auto it = tables_.find(table_name);
  if (it == tables_.end()) {
    return false;
  }

  const TableData& data = it->second;
  std::string key = encode_key(table_name, primary_key);
  auto row_it = data.rows.find(key);
  if (row_it == data.rows.end()) {
    return false;
  }

  row = decode_row(row_it->second, data.schema);
  return true;
}

std::vector<Row> MockEngine::get_by_keys(const std::string& table_name,
                                         const KeySet& keys) {
  std::lock_guard<std::mutex> lock(mutex_);
  std::vector<Row> result;

  auto it = tables_.find(table_name);
  if (it == tables_.end()) {
    return result;
  }

  const TableData& data = it->second;
  for (const auto& pk : keys) {
    std::string key = encode_key(table_name, pk);
    auto row_it = data.rows.find(key);
    if (row_it != data.rows.end()) {
      result.push_back(decode_row(row_it->second, data.schema));
    }
  }

  return result;
}

std::vector<Row> MockEngine::get_by_key_range(const std::string& table_name,
                                              const KeyRange& key_range) {
  std::lock_guard<std::mutex> lock(mutex_);
  std::vector<Row> result;

  auto it = tables_.find(table_name);
  if (it == tables_.end()) {
    return result;
  }

  const TableData& data = it->second;
  std::string prefix = encode_key_prefix(table_name);

  for (const auto& [key, value] : data.rows) {
    // 只处理属于该表的 key
    if (key.find(prefix) != 0) continue;

    // 检查是否在范围内
    if (key_in_range(key, key_range)) {
      result.push_back(decode_row(value, data.schema));
    }
  }

  return result;
}

std::vector<Row> MockEngine::scan_all(const std::string& table_name) {
  std::lock_guard<std::mutex> lock(mutex_);
  std::vector<Row> result;

  auto it = tables_.find(table_name);
  if (it == tables_.end()) {
    return result;
  }

  const TableData& data = it->second;
  std::string prefix = encode_key_prefix(table_name);

  for (const auto& [key, value] : data.rows) {
    if (key.find(prefix) == 0) {
      result.push_back(decode_row(value, data.schema));
    }
  }

  return result;
}

// ============================================================
// TableStore 实现 - 更新
// ============================================================
bool MockEngine::update_by_key(
    const std::string& table_name, const std::string& primary_key,
    const std::vector<std::pair<std::string, Value>>& assignments) {
  std::lock_guard<std::mutex> lock(mutex_);

  auto it = tables_.find(table_name);
  if (it == tables_.end()) {
    return false;
  }

  TableData& data = it->second;
  std::string key = encode_key(table_name, primary_key);
  auto row_it = data.rows.find(key);
  if (row_it == data.rows.end()) {
    return false;
  }

  // 解码、更新、重新编码
  Row row = decode_row(row_it->second, data.schema);
  row = apply_assignments(row, data.schema, assignments);
  row_it->second = encode_row(row, data.schema);

  return true;
}

bool MockEngine::update_by_keys(
    const std::string& table_name, const KeySet& keys,
    const std::vector<std::pair<std::string, Value>>& assignments) {
  bool success = true;
  for (const auto& pk : keys) {
    if (!update_by_key(table_name, pk, assignments)) {
      success = false;
    }
  }
  return success;
}

bool MockEngine::update_by_key_range(
    const std::string& table_name, const KeyRange& key_range,
    const std::vector<std::pair<std::string, Value>>& assignments) {
  std::lock_guard<std::mutex> lock(mutex_);

  auto it = tables_.find(table_name);
  if (it == tables_.end()) {
    return false;
  }

  TableData& data = it->second;
  std::string prefix = encode_key_prefix(table_name);
  bool updated = false;

  for (auto& [key, value] : data.rows) {
    if (key.find(prefix) != 0) continue;
    if (!key_in_range(key, key_range)) continue;

    Row row = decode_row(value, data.schema);
    row = apply_assignments(row, data.schema, assignments);
    value = encode_row(row, data.schema);
    updated = true;
  }

  return updated;
}

// ============================================================
// TableStore 实现 - 删除
// ============================================================
bool MockEngine::delete_by_key(const std::string& table_name,
                               const std::string& primary_key) {
  std::lock_guard<std::mutex> lock(mutex_);

  auto it = tables_.find(table_name);
  if (it == tables_.end()) {
    return false;
  }

  TableData& data = it->second;
  std::string key = encode_key(table_name, primary_key);
  auto row_it = data.rows.find(key);
  if (row_it == data.rows.end()) {
    return false;
  }

  data.rows.erase(row_it);
  return true;
}

bool MockEngine::delete_by_keys(const std::string& table_name,
                                const KeySet& keys) {
  bool success = true;
  for (const auto& pk : keys) {
    if (!delete_by_key(table_name, pk)) {
      success = false;
    }
  }
  return success;
}

bool MockEngine::delete_by_key_range(const std::string& table_name,
                                     const KeyRange& key_range) {
  std::lock_guard<std::mutex> lock(mutex_);

  auto it = tables_.find(table_name);
  if (it == tables_.end()) {
    return false;
  }

  TableData& data = it->second;
  std::string prefix = encode_key_prefix(table_name);
  std::vector<std::string> keys_to_delete;

  for (const auto& [key, _] : data.rows) {
    if (key.find(prefix) != 0) continue;
    if (key_in_range(key, key_range)) {
      keys_to_delete.push_back(key);
    }
  }

  for (const auto& key : keys_to_delete) {
    data.rows.erase(key);
  }

  return !keys_to_delete.empty();
}

// ============================================================
// TableStore 实现 - 统计
// ============================================================
int64_t MockEngine::get_row_count(const std::string& table_name) {
  std::lock_guard<std::mutex> lock(mutex_);

  auto it = tables_.find(table_name);
  if (it == tables_.end()) {
    return 0;
  }

  std::string prefix = encode_key_prefix(table_name);
  int64_t count = 0;
  for (const auto& [key, _] : it->second.rows) {
    if (key.find(prefix) == 0) {
      count++;
    }
  }
  return count;
}

// ============================================================
// 内部辅助方法
// ============================================================

// 编码 Key
std::string MockEngine::encode_key(const std::string& table_name,
                                   const std::string& primary_key) {
  return "data:" + table_name + ":" + primary_key;
}

std::string MockEngine::encode_key_prefix(const std::string& table_name) {
  return "data:" + table_name + ":";
}

// 编码/解码行
std::string MockEngine::encode_row(const Row& row, const TableSchema& schema) {
  std::ostringstream oss;
  for (size_t i = 0; i < row.size(); ++i) {
    if (i > 0) oss << "|";
    oss << row[i].to_string();
  }
  return oss.str();
}

Row MockEngine::decode_row(const std::string& data, const TableSchema& schema) {
  Row row;
  std::stringstream ss(data);
  std::string val_str;
  size_t col_idx = 0;

  while (std::getline(ss, val_str, '|')) {
    if (col_idx < schema.columns.size()) {
      Value val = Value::from_string(val_str, schema.columns[col_idx].type);
      row.push_back(val);
    } else {
      row.push_back(Value());
    }
    col_idx++;
  }

  // 如果行数据少于列数，补充 NULL
  while (row.size() < schema.columns.size()) {
    row.push_back(Value());
  }

  return row;
}

// 主键操作
std::string MockEngine::get_primary_key(const Row& row,
                                        const TableSchema& schema) {
  if (schema.primary_key_index < 0) {
    return "";
  }
  int idx = schema.primary_key_index;
  if (idx >= static_cast<int>(row.size())) {
    return "";
  }
  return row[idx].to_string();
}

std::string MockEngine::generate_next_id(const std::string& table_name) {
  auto it = tables_.find(table_name);
  if (it == tables_.end()) {
    return "";
  }

  // 使用时间戳 + 自增 ID
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

// 行操作
Row MockEngine::build_full_row(const Row& input_row, const TableSchema& schema,
                               const std::vector<std::string>& input_columns) {
  Row full_row;

  if (input_columns.empty()) {
    return input_row;
  }

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
      full_row.push_back(Value());
    }
  }

  return full_row;
}

bool MockEngine::row_matches_assignments(
    const Row& row, const TableSchema& schema,
    const std::vector<std::pair<std::string, Value>>& assignments) {
  // 这里简化处理，实际应该检查条件
  return true;
}

Row MockEngine::apply_assignments(
    const Row& row, const TableSchema& schema,
    const std::vector<std::pair<std::string, Value>>& assignments) {
  Row result = row;

  for (const auto& [col_name, value] : assignments) {
    int idx = schema.get_column_index(col_name);
    if (idx >= 0 && idx < static_cast<int>(result.size())) {
      result[idx] = value;
    }
  }

  return result;
}

// Key 范围解析
bool MockEngine::key_in_range(const std::string& key,
                              const KeyRange& key_range) {
  if (key_range.has_start && key < key_range.start) {
    return false;
  }
  if (key_range.has_end && key >= key_range.end) {
    return false;
  }
  return true;
}

// ============================================================
// 测试辅助
// ============================================================
void MockEngine::dump_table(const std::string& table_name) {
  std::lock_guard<std::mutex> lock(mutex_);

  auto it = tables_.find(table_name);
  if (it == tables_.end()) {
    std::cout << "Table '" << table_name << "' not found" << std::endl;
    return;
  }

  const TableData& data = it->second;
  std::cout << "\n📋 Table: " << table_name << " (" << data.rows.size()
            << " rows)" << std::endl;

  if (data.rows.empty()) {
    std::cout << "  (empty)" << std::endl;
    return;
  }

  // 打印表头
  std::cout << "  ";
  for (const auto& col : data.schema.columns) {
    std::cout << col.name << "(";
    switch (col.type) {
      case DataType::INTEGER:
        std::cout << "int";
        break;
      case DataType::STRING:
        std::cout << "str";
        break;
      case DataType::BOOLEAN:
        std::cout << "bool";
        break;
      default:
        std::cout << "null";
        break;
    }
    std::cout << ") ";
  }
  std::cout << std::endl;
  std::cout << "  " << std::string(50, '-') << std::endl;

  // 打印数据
  std::string prefix = encode_key_prefix(table_name);
  std::vector<std::pair<std::string, std::string>> sorted_rows;
  for (const auto& [key, value] : data.rows) {
    if (key.find(prefix) == 0) {
      sorted_rows.push_back({key, value});
    }
  }
  std::sort(sorted_rows.begin(), sorted_rows.end());

  for (const auto& [key, value] : sorted_rows) {
    Row row = decode_row(value, data.schema);
    std::cout << "  ";
    for (size_t i = 0; i < row.size(); ++i) {
      if (i > 0) std::cout << " ";
      std::cout << row[i].to_string() << " ";
    }
    std::cout << " [key: " << key << "]" << std::endl;
  }
}

void MockEngine::clear() {
  std::lock_guard<std::mutex> lock(mutex_);
  tables_.clear();
}

}  // namespace storage