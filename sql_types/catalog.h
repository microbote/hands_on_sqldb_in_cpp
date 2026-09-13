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
  virtual bool database_exists(const Identifier &db_name) const = 0;
  virtual std::vector<Identifier> list_databases() const = 0;

  // ---- 表元数据 ----
  virtual bool table_exists(const Identifier &db_name,
                            const Identifier &table_name) const = 0;
  virtual std::vector<Identifier>
  list_tables(const Identifier &db_name) const = 0;
  virtual std::optional<TableSchema>
  get_table_schema(const Identifier &db_name,
                   const Identifier &table_name) const = 0;

  // ---- 可选：数据探测（默认"不支持"）----
  //
  // 校验器用它做**执行前**的主键冲突检查（越早发现越好：在事务开头的语句就
  // 报错，比执行到一半再回滚便宜得多）。返回：
  //   true    —— 这一行已经存在
  //   false   —— 确认不存在
  //   nullopt —— 这个 Catalog 不支持探测 / 探测不出结论
  //              （调用方退回执行期检查：Table::insert 本来就会报重复主键）
  virtual std::optional<bool>
  primary_key_exists(const Identifier &db_name, const Identifier &table_name,
                     const Value &primary_key) const {
    (void)db_name;
    (void)table_name;
    (void)primary_key;
    return std::nullopt;
  }

  // ---- DDL（元数据持久化动作） ----
  virtual bool create_database(const Identifier &db_name) = 0;
  virtual bool drop_database(const Identifier &db_name) = 0;
  virtual bool create_table(const Identifier &db_name,
                            const TableSchema &schema) = 0;
  virtual bool drop_table(const Identifier &db_name,
                          const Identifier &table_name) = 0;
};

} // namespace sql
