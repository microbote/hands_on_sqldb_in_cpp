// database_manager.cpp
#include "database_manager.h"

#include <iostream>
#include <sstream>

namespace sql {

DatabaseManager::DatabaseManager(std::shared_ptr<kv::KVEngine> engine)
    : engine_(engine) {
  load_metadata();
}

// ============================================================
// 数据库操作
// ============================================================

bool DatabaseManager::create_database(const std::string& name) {
  if (database_exists(name)) {
    std::cerr << "❌ Database '" << name << "' already exists" << std::endl;
    return false;
  }

  // 1. 更新数据库列表
  auto dbs = list_databases();
  dbs.push_back(name);
  std::string dbs_data = serialize_database_list(dbs);
  if (engine_->put(keys::databases(), dbs_data) != kv::Status::OK) {
    return false;
  }

  // 2. 创建数据库元数据（空表列表）
  if (engine_->put(keys::db_tables(name), "") != kv::Status::OK) {
    // 回滚
    auto old_dbs = list_databases();
    // 移除刚添加的数据库
    auto it = std::find(old_dbs.begin(), old_dbs.end(), name);
    if (it != old_dbs.end()) {
      old_dbs.erase(it);
    }
    engine_->put(keys::databases(), serialize_database_list(old_dbs));
    return false;
  }

  // 3. 更新缓存
  auto db = std::make_shared<Database>(engine_, name);
  database_cache_[name] = db;

  std::cout << "✅ Database '" << name << "' created" << std::endl;
  return true;
}

bool DatabaseManager::drop_database(const std::string& name) {
  if (!database_exists(name)) {
    std::cerr << "❌ Database '" << name << "' does not exist" << std::endl;
    return false;
  }

  // 1. 删除数据库的所有表数据（简化：只删除元数据）
  // 注意：实际应该删除所有表数据，这里只删除元数据

  // 2. 从数据库列表中移除
  auto dbs = list_databases();
  auto it = std::find(dbs.begin(), dbs.end(), name);
  if (it != dbs.end()) {
    dbs.erase(it);
  }
  std::string dbs_data = serialize_database_list(dbs);
  if (engine_->put(keys::databases(), dbs_data) != kv::Status::OK) {
    return false;
  }

  // 3. 删除数据库元数据
  engine_->remove(keys::db_tables(name));

  // 4. 删除所有表的 schema
  auto db = open_database(name);
  if (db) {
    for (const auto& table_name : db->list_tables()) {
      engine_->remove(keys::db_schema(name, table_name));
    }
  }

  // 5. 清除缓存
  database_cache_.erase(name);

  std::cout << "✅ Database '" << name << "' dropped" << std::endl;
  return true;
}

std::shared_ptr<Database> DatabaseManager::open_database(
    const std::string& name) {
  // 检查缓存
  auto it = database_cache_.find(name);
  if (it != database_cache_.end()) {
    return it->second;
  }

  // 检查是否存在
  if (!database_exists(name)) {
    return nullptr;
  }

  // 创建 Database 对象
  auto db = std::make_shared<Database>(engine_, name);
  database_cache_[name] = db;

  std::cout << "✅ Database '" << name << "' opened" << std::endl;
  return db;
}

std::vector<std::string> DatabaseManager::list_databases() const {
  std::string data;
  if (engine_->get(keys::databases(), &data) != kv::Status::OK) {
    return {};
  }
  return deserialize_database_list(data);
}

bool DatabaseManager::database_exists(const std::string& name) const {
  auto dbs = list_databases();
  return std::find(dbs.begin(), dbs.end(), name) != dbs.end();
}

// ============================================================
// 元数据
// ============================================================

bool DatabaseManager::load_metadata() {
  metadata_loaded_ = true;
  return true;
}

// ============================================================
// 序列化/反序列化
// ============================================================

std::string DatabaseManager::serialize_database_list(
    const std::vector<std::string>& dbs) const {
  std::ostringstream oss;
  for (size_t i = 0; i < dbs.size(); ++i) {
    if (i > 0) oss << ",";
    oss << dbs[i];
  }
  return oss.str();
}

std::vector<std::string> DatabaseManager::deserialize_database_list(
    const std::string& data) const {
  std::vector<std::string> dbs;
  std::stringstream ss(data);
  std::string db;
  while (std::getline(ss, db, ',')) {
    if (!db.empty()) {
      dbs.push_back(db);
    }
  }
  return dbs;
}

}  // namespace sql