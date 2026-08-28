// schema.h
#pragma once

#include <string>
#include <vector>

#include "types.h"
#include "value.h"

namespace sql {
class Row;
// ============================================================
// 列定义
// ============================================================
struct ColumnDef {
  std::string name;
  DataType type;
  bool nullable = true;
  bool primary_key = false;

  ColumnDef() = default;
  ColumnDef(const std::string& n, DataType t, bool pk = false, bool nul = true)
      : name(n), type(t), nullable(nul), primary_key(pk) {}
};

// ============================================================
// 表结构
// ============================================================
class TableSchema {
 public:
  TableSchema() = default;
  TableSchema(const std::string& name) : name_(name) {}

  // ----- 属性 -----
  const std::string& name() const { return name_; }
  void set_name(const std::string& name) { name_ = name; }

  const std::vector<ColumnDef>& columns() const { return columns_; }
  std::vector<ColumnDef>& columns() { return columns_; }

  // ----- 列操作 -----
  void add_column(const ColumnDef& col);
  void add_column(const std::string& name, DataType type, bool pk = false,
                  bool nullable = true);

  // ----- 查询 -----
  int column_index(const std::string& name) const;
  const ColumnDef* column(const std::string& name) const;
  std::string primary_key_name() const;
  int primary_key_index() const { return primary_key_index_; }
  void set_primary_key_index(int idx) { primary_key_index_ = idx; }

  // ----- 验证 -----
  bool validate_row(const Row& row) const;

  // ----- 序列化 -----
  std::string serialize() const;
  static TableSchema deserialize(const std::string& data);

 private:
  std::string name_;
  std::vector<ColumnDef> columns_;
  int primary_key_index_ = -1;
};

}  // namespace sql