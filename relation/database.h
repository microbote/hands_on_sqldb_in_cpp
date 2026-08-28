// database.h
#pragma once

#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "key_prefix.h"
#include "schema.h"
#include "storage/kv_engine/kv_engine.h"
#include "table.h"

namespace sql {

// ============================================================
// 数据库抽象
// ============================================================
class Database {
 public:
  Database(std::shared_ptr<kv::KVEngine> engine, const std::string& name);
  ~Database() = default;

  // ----- 属性 -----
  const std::string& name() const { return name_; }

  // ===== 表操作 =====

  // 创建表
  bool create_table(const TableSchema& schema);

  // 删除表
  bool drop_table(const std::string& table_name);

  // 获取表
  std::shared_ptr<Table> get_table(const std::string& table_name);

  // 列出所有表
  std::vector<std::string> list_tables() const;

  // 检查表是否存在
  bool table_exists(const std::string& table_name) const;

  // ===== 元数据 =====
  bool load_metadata();

 private:
  // ----- 序列化/反序列化 -----
  std::string serialize_table_list(
      const std::vector<std::string>& tables) const;
  std::vector<std::string> deserialize_table_list(
      const std::string& data) const;
  std::string serialize_schema(const TableSchema& schema) const;
  TableSchema deserialize_schema(const std::string& data) const;

  // ----- 缓存管理 -----
  void refresh_table_cache();

  std::shared_ptr<kv::KVEngine> engine_;
  std::string name_;
  std::unordered_map<std::string, std::shared_ptr<Table>> table_cache_;
  bool metadata_loaded_ = false;
};

}  // namespace sql