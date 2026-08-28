// database.cpp
#include "database.h"

#include <iostream>
#include <sstream>

namespace sql {

Database::Database(std::shared_ptr<kv::KVEngine> engine,
                   const std::string& name)
    : engine_(engine), name_(name) {
  load_metadata();
}

// ============================================================
// 表操作
// ============================================================

bool Database::create_table(const TableSchema& schema) {
  if (table_exists(schema.name())) {
    std::cerr << "❌ Table '" << schema.name() << "' already exists"
              << std::endl;
    return false;
  }

  // 1. 保存表结构
  std::string schema_key = keys::db_schema(name_, schema.name());
  std::string schema_data = serialize_schema(schema);
  if (engine_->put(schema_key, schema_data) != kv::Status::OK) {
    return false;
  }

  // 2. 更新表列表
  auto tables = list_tables();
  tables.push_back(schema.name());
  std::string tables_data = serialize_table_list(tables);
  if (engine_->put(keys::db_tables(name_), tables_data) != kv::Status::OK) {
    // 回滚
    engine_->remove(schema_key);
    return false;
  }

  // 3. 更新缓存
  auto table = std::make_shared<Table>(engine_, name_, schema);
  table_cache_[schema.name()] = table;

  std::cout << "✅ Table '" << schema.name() << "' created in database '"
            << name_ << "'" << std::endl;
  return true;
}

bool Database::drop_table(const std::string& table_name) {
  if (!table_exists(table_name)) {
    std::cerr << "❌ Table '" << table_name << "' does not exist" << std::endl;
    return false;
  }

  // 1. 删除表结构
  std::string schema_key = keys::db_schema(name_, table_name);
  if (engine_->remove(schema_key) != kv::Status::OK) {
    return false;
  }

  // 2. 更新表列表
  auto tables = list_tables();
  auto it = std::find(tables.begin(), tables.end(), table_name);
  if (it != tables.end()) {
    tables.erase(it);
  }
  std::string tables_data = serialize_table_list(tables);
  if (engine_->put(keys::db_tables(name_), tables_data) != kv::Status::OK) {
    // 回滚
    engine_->put(schema_key, serialize_schema(get_table(table_name)->schema()));
    return false;
  }

  // 3. 清除缓存
  table_cache_.erase(table_name);

  std::cout << "✅ Table '" << table_name << "' dropped from database '"
            << name_ << "'" << std::endl;
  return true;
}

std::shared_ptr<Table> Database::get_table(const std::string& table_name) {
  // 检查缓存
  auto it = table_cache_.find(table_name);
  if (it != table_cache_.end()) {
    return it->second;
  }

  // 检查表是否存在
  if (!table_exists(table_name)) {
    return nullptr;
  }

  // 从元数据加载
  std::string schema_key = keys::db_schema(name_, table_name);
  std::string schema_data;
  if (engine_->get(schema_key, &schema_data) != kv::Status::OK) {
    return nullptr;
  }

  TableSchema schema = deserialize_schema(schema_data);
  auto table = std::make_shared<Table>(engine_, name_, schema);
  table_cache_[table_name] = table;

  return table;
}

std::vector<std::string> Database::list_tables() const {
  std::string data;
  if (engine_->get(keys::db_tables(name_), &data) != kv::Status::OK) {
    return {};
  }
  return deserialize_table_list(data);
}

bool Database::table_exists(const std::string& table_name) const {
  auto tables = list_tables();
  return std::find(tables.begin(), tables.end(), table_name) != tables.end();
}

// ============================================================
// 元数据
// ============================================================

bool Database::load_metadata() {
  metadata_loaded_ = true;
  return true;
}

// ============================================================
// 序列化/反序列化
// ============================================================

std::string Database::serialize_table_list(
    const std::vector<std::string>& tables) const {
  std::ostringstream oss;
  for (size_t i = 0; i < tables.size(); ++i) {
    if (i > 0) oss << ",";
    oss << tables[i];
  }
  return oss.str();
}

std::vector<std::string> Database::deserialize_table_list(
    const std::string& data) const {
  std::vector<std::string> tables;
  std::stringstream ss(data);
  std::string table;
  while (std::getline(ss, table, ',')) {
    if (!table.empty()) {
      tables.push_back(table);
    }
  }
  return tables;
}

std::string Database::serialize_schema(const TableSchema& schema) const {
  return schema.serialize();
}

TableSchema Database::deserialize_schema(const std::string& data) const {
  return TableSchema::deserialize(data);
}

}  // namespace sql