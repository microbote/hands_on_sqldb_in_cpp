// leveldb_engine.cpp
#include "leveldb_engine.h"

#include <leveldb/comparator.h>
#include <leveldb/options.h>
#include <leveldb/status.h>

#include <chrono>
#include <iomanip>
#include <iostream>
#include <random>
#include <sstream>

namespace storage {

// ============================================================
// 静态常量
// ============================================================
const std::string LevelDBEngine::META_TABLES_KEY = "__tables__";
const std::string LevelDBEngine::META_SCHEMA_PREFIX = "__schema__";
const std::string LevelDBEngine::DATA_PREFIX = "data:";

// ============================================================
// 构造/析构
// ============================================================
LevelDBEngine::LevelDBEngine(const std::string& path)
    : db_path_(path), is_open_(false) {
  leveldb::Options options;
  options.create_if_missing = true;

  leveldb::DB* db_ptr = nullptr;
  leveldb::Status status = leveldb::DB::Open(options, path, &db_ptr);

  if (!status.ok()) {
    throw std::runtime_error("Failed to open LevelDB: " + status.ToString());
  }

  db_.reset(db_ptr);
  is_open_ = true;

  // 加载 schema 缓存
  load_schema_cache();
}

LevelDBEngine::~LevelDBEngine() { close(); }

// ============================================================
// KVStore 接口实现
// ============================================================
bool LevelDBEngine::put(const std::string& key, const std::string& value) {
  if (!is_open_) return false;

  std::lock_guard<std::mutex> lock(mutex_);
  leveldb::WriteOptions options;
  options.sync = false;

  leveldb::Status status = db_->Put(options, key, value);
  return status.ok();
}

bool LevelDBEngine::get(const std::string& key, std::string& value) {
  if (!is_open_) return false;

  std::lock_guard<std::mutex> lock(mutex_);
  leveldb::ReadOptions options;

  leveldb::Status status = db_->Get(options, key, &value);
  return status.ok();
}

bool LevelDBEngine::del(const std::string& key) {
  if (!is_open_) return false;

  std::lock_guard<std::mutex> lock(mutex_);
  leveldb::WriteOptions options;
  options.sync = false;

  leveldb::Status status = db_->Delete(options, key);
  return status.ok();
}

bool LevelDBEngine::batch_put(
    const std::vector<std::pair<std::string, std::string>>& kv_pairs) {
  if (!is_open_ || kv_pairs.empty()) return false;

  std::lock_guard<std::mutex> lock(mutex_);
  leveldb::WriteBatch batch;

  for (const auto& [key, value] : kv_pairs) {
    batch.Put(key, value);
  }

  leveldb::WriteOptions options;
  options.sync = false;
  leveldb::Status status = db_->Write(options, &batch);
  return status.ok();
}

std::vector<std::pair<std::string, std::string>> LevelDBEngine::scan(
    const std::string& start, const std::string& end) {
  std::vector<std::pair<std::string, std::string>> result;
  if (!is_open_) return result;

  std::lock_guard<std::mutex> lock(mutex_);
  leveldb::ReadOptions options;

  leveldb::Iterator* it = db_->NewIterator(options);
  for (it->Seek(start); it->Valid(); it->Next()) {
    std::string key = it->key().ToString();
    if (!end.empty() && key >= end) {
      break;
    }
    result.push_back({key, it->value().ToString()});
  }

  delete it;
  return result;
}

std::vector<std::pair<std::string, std::string>> LevelDBEngine::scan_prefix(
    const std::string& prefix) {
  std::vector<std::pair<std::string, std::string>> result;
  if (!is_open_) return result;

  std::lock_guard<std::mutex> lock(mutex_);
  leveldb::ReadOptions options;

  leveldb::Iterator* it = db_->NewIterator(options);
  for (it->Seek(prefix); it->Valid(); it->Next()) {
    std::string key = it->key().ToString();
    if (key.find(prefix) != 0) {
      break;
    }
    result.push_back({key, it->value().ToString()});
  }

  delete it;
  return result;
}

void LevelDBEngine::flush() {
  if (!is_open_) return;
  // LevelDB 自动管理 flush，无需手动操作
}

void LevelDBEngine::close() {
  if (!is_open_) return;

  std::lock_guard<std::mutex> lock(mutex_);
  if (db_) {
    db_.reset();
  }
  is_open_ = false;
}

// ============================================================
// TableStore 实现 - 表操作
// ============================================================
bool LevelDBEngine::create_table(const TableSchema& schema) {
  if (!is_open_) return false;

  std::lock_guard<std::mutex> lock(mutex_);

  // 检查表是否已存在
  if (table_exists(schema.table_name)) {
    return false;
  }

  // 获取现有表列表
  std::vector<std::string> tables = load_table_list();
  tables.push_back(schema.table_name);
  save_table_list(tables);

  // 保存表结构
  save_schema(schema);

  // 更新缓存
  schema_cache_[schema.table_name] = schema;

  return true;
}

bool LevelDBEngine::drop_table(const std::string& table_name) {
  if (!is_open_) return false;

  std::lock_guard<std::mutex> lock(mutex_);

  if (!table_exists(table_name)) {
    return false;
  }

  // 删除表数据
  std::string prefix = encode_key_prefix(table_name);
  leveldb::WriteBatch batch;

  leveldb::ReadOptions read_options;
  leveldb::Iterator* it = db_->NewIterator(read_options);
  for (it->Seek(prefix); it->Valid(); it->Next()) {
    std::string key = it->key().ToString();
    if (key.find(prefix) != 0) {
      break;
    }
    batch.Delete(key);
  }
  delete it;

  // 删除表结构
  std::string schema_key = get_schema_meta_key(table_name);
  batch.Delete(schema_key);

  // 从表列表中移除
  std::vector<std::string> tables = load_table_list();
  auto new_end = std::remove(tables.begin(), tables.end(), table_name);
  tables.erase(new_end, tables.end());
  save_table_list(tables);

  // 执行删除
  leveldb::WriteOptions write_options;
  write_options.sync = false;
  leveldb::Status status = db_->Write(write_options, &batch);

  if (status.ok()) {
    schema_cache_.erase(table_name);
    return true;
  }

  return false;
}

bool LevelDBEngine::table_exists(const std::string& table_name) {
  if (!is_open_) return false;

  std::lock_guard<std::mutex> lock(mutex_);

  // 先检查缓存
  if (schema_cache_.find(table_name) != schema_cache_.end()) {
    return true;
  }

  // 检查表列表
  std::vector<std::string> tables = load_table_list();
  return std::find(tables.begin(), tables.end(), table_name) != tables.end();
}

std::vector<std::string> LevelDBEngine::list_tables() {
  if (!is_open_) return {};

  std::lock_guard<std::mutex> lock(mutex_);
  return load_table_list();
}

TableSchema LevelDBEngine::get_table_schema(const std::string& table_name) {
  if (!is_open_) return TableSchema();

  std::lock_guard<std::mutex> lock(mutex_);

  // 检查缓存
  auto it = schema_cache_.find(table_name);
  if (it != schema_cache_.end()) {
    return it->second;
  }

  // 从磁盘加载
  return load_schema(table_name);
}

// ============================================================
// TableStore 实现 - 插入
// ============================================================
bool LevelDBEngine::insert(const std::string& table_name, const Row& row) {
  if (!is_open_) return false;

  std::lock_guard<std::mutex> lock(mutex_);

  auto schema_it = schema_cache_.find(table_name);
  if (schema_it == schema_cache_.end()) {
    return false;
  }

  const TableSchema& schema = schema_it->second;

  // 构建完整行
  Row full_row = row;

  // 如果主键为空，自动生成
  if (schema.primary_key_index >= 0) {
    int pk_idx = schema.primary_key_index;
    if (pk_idx >= static_cast<int>(full_row.size()) ||
        full_row[pk_idx].type == DataType::NULL_TYPE) {
      std::string pk = generate_next_id(table_name);
      while (static_cast<int>(full_row.size()) <= pk_idx) {
        full_row.push_back(Value());
      }
      full_row[pk_idx] = Value(pk);
    }
  }

  // 获取主键
  std::string pk = get_primary_key(full_row, schema);
  if (pk.empty()) {
    return false;
  }

  // 编码并存储
  std::string key = encode_key(table_name, pk);
  std::string value = encode_row(full_row, schema);

  leveldb::WriteOptions options;
  options.sync = false;
  leveldb::Status status = db_->Put(options, key, value);

  return status.ok();
}

bool LevelDBEngine::insert_batch(const std::string& table_name,
                                 const std::vector<Row>& rows) {
  if (!is_open_ || rows.empty()) return false;

  std::lock_guard<std::mutex> lock(mutex_);

  auto schema_it = schema_cache_.find(table_name);
  if (schema_it == schema_cache_.end()) {
    return false;
  }

  const TableSchema& schema = schema_it->second;
  leveldb::WriteBatch batch;

  for (const auto& row : rows) {
    Row full_row = row;

    // 自动生成主键
    if (schema.primary_key_index >= 0) {
      int pk_idx = schema.primary_key_index;
      if (pk_idx >= static_cast<int>(full_row.size()) ||
          full_row[pk_idx].type == DataType::NULL_TYPE) {
        std::string pk = generate_next_id(table_name);
        while (static_cast<int>(full_row.size()) <= pk_idx) {
          full_row.push_back(Value());
        }
        full_row[pk_idx] = Value(pk);
      }
    }

    std::string pk = get_primary_key(full_row, schema);
    if (!pk.empty()) {
      std::string key = encode_key(table_name, pk);
      std::string value = encode_row(full_row, schema);
      batch.Put(key, value);
    }
  }

  leveldb::WriteOptions options;
  options.sync = false;
  leveldb::Status status = db_->Write(options, &batch);

  return status.ok();
}

// ============================================================
// TableStore 实现 - 查询
// ============================================================
bool LevelDBEngine::get_by_key(const std::string& table_name,
                               const std::string& primary_key, Row& row) {
  if (!is_open_) return false;

  std::lock_guard<std::mutex> lock(mutex_);

  auto schema_it = schema_cache_.find(table_name);
  if (schema_it == schema_cache_.end()) {
    return false;
  }

  std::string key = encode_key(table_name, primary_key);
  std::string value;

  leveldb::ReadOptions options;
  leveldb::Status status = db_->Get(options, key, &value);

  if (!status.ok()) {
    return false;
  }

  row = decode_row(value, schema_it->second);
  return true;
}

std::vector<Row> LevelDBEngine::get_by_keys(const std::string& table_name,
                                            const KeySet& keys) {
  std::vector<Row> result;
  if (!is_open_ || keys.empty()) return result;

  std::lock_guard<std::mutex> lock(mutex_);

  auto schema_it = schema_cache_.find(table_name);
  if (schema_it == schema_cache_.end()) {
    return result;
  }

  const TableSchema& schema = schema_it->second;
  leveldb::ReadOptions options;

  for (const auto& pk : keys) {
    std::string key = encode_key(table_name, pk);
    std::string value;
    leveldb::Status status = db_->Get(options, key, &value);
    if (status.ok()) {
      result.push_back(decode_row(value, schema));
    }
  }

  return result;
}

std::vector<Row> LevelDBEngine::get_by_key_range(const std::string& table_name,
                                                 const KeyRange& key_range) {
  std::vector<Row> result;
  if (!is_open_) return result;

  std::lock_guard<std::mutex> lock(mutex_);

  auto schema_it = schema_cache_.find(table_name);
  if (schema_it == schema_cache_.end()) {
    return result;
  }

  const TableSchema& schema = schema_it->second;
  std::string prefix = encode_key_prefix(table_name);

  // 构建扫描范围
  std::string start = key_range.has_start ? key_range.start : prefix;
  std::string end = key_range.has_end ? key_range.end : prefix + "\xFF";

  leveldb::ReadOptions options;
  leveldb::Iterator* it = db_->NewIterator(options);

  for (it->Seek(start); it->Valid(); it->Next()) {
    std::string key = it->key().ToString();
    if (key >= end) {
      break;
    }
    if (key.find(prefix) == 0) {
      result.push_back(decode_row(it->value().ToString(), schema));
    }
  }

  delete it;
  return result;
}

std::vector<Row> LevelDBEngine::scan_all(const std::string& table_name) {
  std::vector<Row> result;
  if (!is_open_) return result;

  std::lock_guard<std::mutex> lock(mutex_);

  auto schema_it = schema_cache_.find(table_name);
  if (schema_it == schema_cache_.end()) {
    return result;
  }

  const TableSchema& schema = schema_it->second;
  std::string prefix = encode_key_prefix(table_name);

  leveldb::ReadOptions options;
  leveldb::Iterator* it = db_->NewIterator(options);

  for (it->Seek(prefix); it->Valid(); it->Next()) {
    std::string key = it->key().ToString();
    if (key.find(prefix) != 0) {
      break;
    }
    result.push_back(decode_row(it->value().ToString(), schema));
  }

  delete it;
  return result;
}

// ============================================================
// TableStore 实现 - 更新
// ============================================================
bool LevelDBEngine::update_by_key(
    const std::string& table_name, const std::string& primary_key,
    const std::vector<std::pair<std::string, Value>>& assignments) {
  if (!is_open_) return false;

  std::lock_guard<std::mutex> lock(mutex_);

  auto schema_it = schema_cache_.find(table_name);
  if (schema_it == schema_cache_.end()) {
    return false;
  }

  const TableSchema& schema = schema_it->second;
  std::string key = encode_key(table_name, primary_key);
  std::string value_data;

  leveldb::ReadOptions read_options;
  leveldb::Status status = db_->Get(read_options, key, &value_data);

  if (!status.ok()) {
    return false;
  }

  Row row = decode_row(value_data, schema);
  row = apply_assignments(row, schema, assignments);

  std::string new_value = encode_row(row, schema);

  leveldb::WriteOptions write_options;
  write_options.sync = false;
  status = db_->Put(write_options, key, new_value);

  return status.ok();
}

bool LevelDBEngine::update_by_keys(
    const std::string& table_name, const KeySet& keys,
    const std::vector<std::pair<std::string, Value>>& assignments) {
  if (!is_open_ || keys.empty()) return false;

  bool success = true;
  for (const auto& pk : keys) {
    if (!update_by_key(table_name, pk, assignments)) {
      success = false;
    }
  }
  return success;
}

bool LevelDBEngine::update_by_key_range(
    const std::string& table_name, const KeyRange& key_range,
    const std::vector<std::pair<std::string, Value>>& assignments) {
  if (!is_open_) return false;

  std::lock_guard<std::mutex> lock(mutex_);

  auto schema_it = schema_cache_.find(table_name);
  if (schema_it == schema_cache_.end()) {
    return false;
  }

  const TableSchema& schema = schema_it->second;
  std::string prefix = encode_key_prefix(table_name);

  std::string start = key_range.has_start ? key_range.start : prefix;
  std::string end = key_range.has_end ? key_range.end : prefix + "\xFF";

  leveldb::WriteBatch batch;
  leveldb::ReadOptions read_options;
  leveldb::Iterator* it = db_->NewIterator(read_options);

  bool updated = false;
  for (it->Seek(start); it->Valid(); it->Next()) {
    std::string key = it->key().ToString();
    if (key >= end) {
      break;
    }
    if (key.find(prefix) != 0) {
      continue;
    }

    Row row = decode_row(it->value().ToString(), schema);
    row = apply_assignments(row, schema, assignments);
    std::string new_value = encode_row(row, schema);
    batch.Put(key, new_value);
    updated = true;
  }

  delete it;

  if (updated) {
    leveldb::WriteOptions write_options;
    write_options.sync = false;
    leveldb::Status status = db_->Write(write_options, &batch);
    return status.ok();
  }

  return false;
}

// ============================================================
// TableStore 实现 - 删除
// ============================================================
bool LevelDBEngine::delete_by_key(const std::string& table_name,
                                  const std::string& primary_key) {
  if (!is_open_) return false;

  std::lock_guard<std::mutex> lock(mutex_);

  // 检查表是否存在
  if (schema_cache_.find(table_name) == schema_cache_.end()) {
    return false;
  }

  std::string key = encode_key(table_name, primary_key);

  leveldb::WriteOptions options;
  options.sync = false;
  leveldb::Status status = db_->Delete(options, key);

  return status.ok();
}

bool LevelDBEngine::delete_by_keys(const std::string& table_name,
                                   const KeySet& keys) {
  if (!is_open_ || keys.empty()) return false;

  std::lock_guard<std::mutex> lock(mutex_);

  if (schema_cache_.find(table_name) == schema_cache_.end()) {
    return false;
  }

  leveldb::WriteBatch batch;
  for (const auto& pk : keys) {
    std::string key = encode_key(table_name, pk);
    batch.Delete(key);
  }

  leveldb::WriteOptions options;
  options.sync = false;
  leveldb::Status status = db_->Write(options, &batch);

  return status.ok();
}

bool LevelDBEngine::delete_by_key_range(const std::string& table_name,
                                        const KeyRange& key_range) {
  if (!is_open_) return false;

  std::lock_guard<std::mutex> lock(mutex_);

  if (schema_cache_.find(table_name) == schema_cache_.end()) {
    return false;
  }

  std::string prefix = encode_key_prefix(table_name);
  std::string start = key_range.has_start ? key_range.start : prefix;
  std::string end = key_range.has_end ? key_range.end : prefix + "\xFF";

  leveldb::WriteBatch batch;
  leveldb::ReadOptions read_options;
  leveldb::Iterator* it = db_->NewIterator(read_options);

  bool deleted = false;
  for (it->Seek(start); it->Valid(); it->Next()) {
    std::string key = it->key().ToString();
    if (key >= end) {
      break;
    }
    if (key.find(prefix) == 0) {
      batch.Delete(key);
      deleted = true;
    }
  }

  delete it;

  if (deleted) {
    leveldb::WriteOptions write_options;
    write_options.sync = false;
    leveldb::Status status = db_->Write(write_options, &batch);
    return status.ok();
  }

  return false;
}

// ============================================================
// TableStore 实现 - 统计
// ============================================================
int64_t LevelDBEngine::get_row_count(const std::string& table_name) {
  if (!is_open_) return 0;

  std::lock_guard<std::mutex> lock(mutex_);

  if (schema_cache_.find(table_name) == schema_cache_.end()) {
    return 0;
  }

  std::string prefix = encode_key_prefix(table_name);
  leveldb::ReadOptions options;
  leveldb::Iterator* it = db_->NewIterator(options);

  int64_t count = 0;
  for (it->Seek(prefix); it->Valid(); it->Next()) {
    std::string key = it->key().ToString();
    if (key.find(prefix) != 0) {
      break;
    }
    count++;
  }

  delete it;
  return count;
}

// ============================================================
// 内部辅助方法 - 编码/解码
// ============================================================
std::string LevelDBEngine::encode_key(const std::string& table_name,
                                      const std::string& primary_key) {
  return DATA_PREFIX + table_name + ":" + primary_key;
}

std::string LevelDBEngine::encode_key_prefix(const std::string& table_name) {
  return DATA_PREFIX + table_name + ":";
}

std::string LevelDBEngine::encode_row(const Row& row,
                                      const TableSchema& schema) {
  std::ostringstream oss;
  for (size_t i = 0; i < row.size(); ++i) {
    if (i > 0) oss << "|";
    oss << row[i].to_string();
  }
  return oss.str();
}

Row LevelDBEngine::decode_row(const std::string& data,
                              const TableSchema& schema) {
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

  while (row.size() < schema.columns.size()) {
    row.push_back(Value());
  }

  return row;
}

// ============================================================
// 内部辅助方法 - 主键操作
// ============================================================
std::string LevelDBEngine::get_primary_key(const Row& row,
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

std::string LevelDBEngine::generate_next_id(const std::string& table_name) {
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

bool LevelDBEngine::has_primary_key(const Row& row, const TableSchema& schema) {
  std::string pk = get_primary_key(row, schema);
  return !pk.empty();
}

// ============================================================
// 内部辅助方法 - 行操作
// ============================================================
Row LevelDBEngine::build_full_row(
    const Row& input_row, const TableSchema& schema,
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

Row LevelDBEngine::apply_assignments(
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

// ============================================================
// 内部辅助方法 - Key 范围
// ============================================================
bool LevelDBEngine::key_in_range(const std::string& key,
                                 const KeyRange& key_range) {
  if (key_range.has_start && key < key_range.start) {
    return false;
  }
  if (key_range.has_end && key >= key_range.end) {
    return false;
  }
  return true;
}

std::string LevelDBEngine::extract_pk_from_key(const std::string& key,
                                               const std::string& table_name) {
  std::string prefix = encode_key_prefix(table_name);
  if (key.find(prefix) != 0) {
    return "";
  }
  return key.substr(prefix.length());
}

// ============================================================
// 内部辅助方法 - 元数据操作
// ============================================================
std::string LevelDBEngine::get_tables_meta_key() { return META_TABLES_KEY; }

std::string LevelDBEngine::get_schema_meta_key(const std::string& table_name) {
  return META_SCHEMA_PREFIX + ":" + table_name;
}

void LevelDBEngine::load_schema_cache() {
  schema_cache_.clear();

  std::vector<std::string> tables = load_table_list();
  for (const auto& table_name : tables) {
    TableSchema schema = load_schema(table_name);
    if (!schema.table_name.empty()) {
      schema_cache_[table_name] = schema;
    }
  }
}

void LevelDBEngine::save_table_list(const std::vector<std::string>& tables) {
  std::string value;
  for (size_t i = 0; i < tables.size(); ++i) {
    if (i > 0) value += ",";
    value += tables[i];
  }

  leveldb::WriteOptions options;
  options.sync = false;
  db_->Put(options, META_TABLES_KEY, value);
}

std::vector<std::string> LevelDBEngine::load_table_list() {
  std::vector<std::string> tables;
  std::string value;

  leveldb::ReadOptions options;
  leveldb::Status status = db_->Get(options, META_TABLES_KEY, &value);

  if (!status.ok()) {
    return tables;
  }

  std::stringstream ss(value);
  std::string table;
  while (std::getline(ss, table, ',')) {
    if (!table.empty()) {
      tables.push_back(table);
    }
  }

  return tables;
}

void LevelDBEngine::save_schema(const TableSchema& schema) {
  std::string key = get_schema_meta_key(schema.table_name);
  std::ostringstream oss;

  oss << schema.table_name << "|";
  oss << schema.primary_key_index << "|";
  for (size_t i = 0; i < schema.columns.size(); ++i) {
    if (i > 0) oss << ",";
    oss << schema.columns[i].name << ":";
    oss << static_cast<int>(schema.columns[i].type) << ":";
    oss << (schema.columns[i].nullable ? "1" : "0") << ":";
    oss << (schema.columns[i].primary_key ? "1" : "0");
  }

  leveldb::WriteOptions options;
  options.sync = false;
  db_->Put(options, key, oss.str());
}

TableSchema LevelDBEngine::load_schema(const std::string& table_name) {
  TableSchema schema;
  std::string key = get_schema_meta_key(table_name);
  std::string value;

  leveldb::ReadOptions options;
  leveldb::Status status = db_->Get(options, key, &value);

  if (!status.ok()) {
    return schema;
  }

  std::stringstream ss(value);
  std::string token;

  // 解析:
  // table_name|primary_key_index|col1:type:nullable:pk,col2:type:nullable:pk,...
  std::getline(ss, token, '|');
  schema.table_name = token;

  std::getline(ss, token, '|');
  schema.primary_key_index = std::stoi(token);

  while (std::getline(ss, token, ',')) {
    if (token.empty()) continue;

    std::stringstream col_ss(token);
    std::string part;
    ColumnDef col;

    std::getline(col_ss, part, ':');
    col.name = part;

    std::getline(col_ss, part, ':');
    col.type = static_cast<DataType>(std::stoi(part));

    std::getline(col_ss, part, ':');
    col.nullable = (part == "1");

    std::getline(col_ss, part, ':');
    col.primary_key = (part == "1");

    schema.columns.push_back(col);
  }

  return schema;
}

// ============================================================
// 工厂实现
// ============================================================
std::unique_ptr<StorageEngine> StorageEngineFactory::create_engine(
    const StorageOptions& options) {
  switch (options.type) {
    case StorageType::MOCK:
      return std::make_unique<MockEngine>();
    case StorageType::LEVELDB:
      if (options.path.empty()) {
        throw std::runtime_error("LevelDB path is empty");
      }
      return std::make_unique<LevelDBEngine>(options.path);
    default:
      return std::make_unique<MockEngine>();
  }
}

}  // namespace storage