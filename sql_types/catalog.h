#pragma once

#include <optional>
#include <string>
#include <vector>

#include "schema.h"

namespace sql {

// 纯逻辑元数据接口；不含 kv::KVEngine、Table、Cursor
class Catalog {
 public:
  virtual ~Catalog() = default;

  // ---- 数据库元数据 ----
  virtual bool database_exists(const std::string& db_name) const = 0;
  virtual std::vector<std::string> list_databases() const = 0;

  // ---- 表元数据 ----
  virtual bool table_exists(const std::string& db_name,
                            const std::string& table_name) const = 0;
  virtual std::vector<std::string> list_tables(
      const std::string& db_name) const = 0;
  virtual std::optional<TableSchema> get_table_schema(
      const std::string& db_name,
      const std::string& table_name) const = 0;

  // ---- DDL（元数据持久化动作） ----
  virtual bool create_database(const std::string& db_name) = 0;
  virtual bool drop_database(const std::string& db_name) = 0;
  virtual bool create_table(const std::string& db_name,
                            const TableSchema& schema) = 0;
  virtual bool drop_table(const std::string& db_name,
                          const std::string& table_name) = 0;
};

}  // namespace sql