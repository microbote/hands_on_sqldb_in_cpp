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

  virtual bool is_open() const = 0;
  // 当前选中的数据库（未选中时返回空 Identifier）
  virtual Identifier current_database() const = 0;
  // ---- 数据库元数据 ----
  virtual bool database_exists(const Identifier& db_name) const = 0;
  virtual std::vector<Identifier> list_databases() const = 0;

  // ---- 表元数据 ----
  virtual bool table_exists(const Identifier& db_name,
                            const Identifier& table_name) const = 0;
  virtual std::vector<Identifier> list_tables(
      const Identifier& db_name) const = 0;
  virtual std::optional<TableSchema> get_table_schema(
      const Identifier& db_name,
      const Identifier& table_name) const = 0;

  // ---- DDL（元数据持久化动作） ----
  virtual bool create_database(const Identifier& db_name) = 0;
  virtual bool drop_database(const Identifier& db_name) = 0;
  virtual bool create_table(const Identifier& db_name,
                            const TableSchema& schema) = 0;
  virtual bool drop_table(const Identifier& db_name,
                          const Identifier& table_name) = 0;
};

}  // namespace sql
