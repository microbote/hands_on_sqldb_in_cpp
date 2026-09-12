// tests/test_fwk/memory_catalog.h
//
// 测试用的内存 Catalog（各模块共享）：实现 sql::Catalog 的全部接口，
// 支持"当前数据库"（USE 语义）与基本的 DDL 动作。
// 原来放在 tests/test_statement/ 下，statement 与 planner 两套测试都要用，
// 因此挪到 test_fwk（共享测试辅助），include 路径由 test_fwk 接口库提供。
#pragma once

#include <map>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "sql_types/catalog.h"
#include "sql_types/identifier.h"
#include "sql_types/schema.h"

class MemoryCatalog : public sql::Catalog {
public:
  bool is_open() const override { return open_; }
  sql::Identifier current_database() const override { return current_db_; }

  bool database_exists(const sql::Identifier &db_name) const override {
    return databases_.find(db_name) != databases_.end();
  }

  std::vector<sql::Identifier> list_databases() const override {
    std::vector<sql::Identifier> names;
    for (const auto &entry : databases_) {
      names.push_back(entry.first);
    }
    return names;
  }

  bool table_exists(const sql::Identifier &db_name,
                    const sql::Identifier &table_name) const override {
    const auto db = databases_.find(db_name);
    return db != databases_.end() &&
           db->second.find(table_name) != db->second.end();
  }

  std::vector<sql::Identifier>
  list_tables(const sql::Identifier &db_name) const override {
    std::vector<sql::Identifier> names;
    const auto db = databases_.find(db_name);
    if (db != databases_.end()) {
      for (const auto &entry : db->second) {
        names.push_back(entry.first);
      }
    }
    return names;
  }

  std::optional<sql::TableSchema>
  get_table_schema(const sql::Identifier &db_name,
                   const sql::Identifier &table_name) const override {
    const auto db = databases_.find(db_name);
    if (db == databases_.end()) {
      return std::nullopt;
    }
    const auto table = db->second.find(table_name);
    if (table == db->second.end()) {
      return std::nullopt;
    }
    return table->second;
  }

  bool create_database(const sql::Identifier &db_name) override {
    if (db_name.empty()) {
      return false;
    }
    if (databases_.find(db_name) != databases_.end()) {
      return false;
    }
    databases_.emplace(db_name, TableMap{});
    return true;
  }

  bool drop_database(const sql::Identifier &db_name) override {
    if (databases_.erase(db_name) == 0) {
      return false;
    }
    if (current_db_ == db_name) {
      current_db_ = sql::Identifier();
    }
    return true;
  }

  bool create_table(const sql::Identifier &db_name,
                    const sql::TableSchema &schema) override {
    auto db = databases_.find(db_name);
    if (db == databases_.end()) {
      return false;
    }
    return db->second.emplace(schema.table_name(), schema).second;
  }

  bool drop_table(const sql::Identifier &db_name,
                  const sql::Identifier &table_name) override {
    auto db = databases_.find(db_name);
    if (db == databases_.end()) {
      return false;
    }
    return db->second.erase(table_name) > 0;
  }

  // ---- 测试便利方法（不属于 Catalog 接口）----
  void set_open(bool open) { open_ = open; }

  bool use_database(const sql::Identifier &db_name) {
    if (!database_exists(db_name)) {
      return false;
    }
    current_db_ = db_name;
    return true;
  }

  void reset() {
    databases_.clear();
    current_db_ = sql::Identifier();
    open_ = false;
  }

private:
  using TableMap = std::unordered_map<sql::Identifier, sql::TableSchema,
                                      sql::IdentifierHash>;
  std::unordered_map<sql::Identifier, TableMap, sql::IdentifierHash> databases_;
  sql::Identifier current_db_;
  bool open_ = false;
};

// 构造一个测试用的 users 表 schema
inline sql::TableSchema make_users_schema() {
  sql::TableSchema schema(sql::Identifier("users"));
  schema.add_column(sql::Identifier("id"), sql::DataType::INT, true, false);
  schema.add_column(sql::Identifier("name"), sql::DataType::VARCHAR, 32u, false,
                    true);
  schema.add_column(sql::Identifier("age"), sql::DataType::INT, false, true);
  return schema;
}
