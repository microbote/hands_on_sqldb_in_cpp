// database_manager.h
#pragma once

#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "database.h"
#include "key_prefix.h"
#include "storage/kv_engine/kv_engine.h"

namespace sql {

// ============================================================
// 数据库管理器
// ============================================================
class DatabaseManager {
 public:
  explicit DatabaseManager(std::shared_ptr<kv::KVEngine> engine);
  ~DatabaseManager() = default;

  // ===== 数据库操作 =====

  // 创建数据库
  bool create_database(const std::string& name);

  // 删除数据库
  bool drop_database(const std::string& name);

  // 打开数据库
  std::shared_ptr<Database> open_database(const std::string& name);

  // 列出所有数据库
  std::vector<std::string> list_databases() const;

  // 检查数据库是否存在
  bool database_exists(const std::string& name) const;

  // ===== 元数据 =====
  bool load_metadata();

 private:
  // ----- 序列化/反序列化 -----
  std::string serialize_database_list(
      const std::vector<std::string>& dbs) const;
  std::vector<std::string> deserialize_database_list(
      const std::string& data) const;

  // ----- 缓存管理 -----
  void refresh_database_cache();

  std::shared_ptr<kv::KVEngine> engine_;
  std::unordered_map<std::string, std::shared_ptr<Database>> database_cache_;
  bool metadata_loaded_ = false;
};

}  // namespace sql