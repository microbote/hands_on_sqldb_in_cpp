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
// 错误码
// ============================================================
enum class SchemaError {
  OK = 0,
  DUPLICATE_PRIMARY_KEY,
  NO_PRIMARY_KEY,
  DUPLICATE_COLUMN_NAME,
  INVALID_ROW,
  COLUMN_NOT_FOUND,
  COLUMN_ATTR_NULL_MISMATCH,
  COLUMN_SIZE_MISMATCH,
  COLUMN_TYPE_MISMATCH
};

// ============================================================
// 表结构
// ============================================================
class TableSchema {
 public:
  TableSchema() = default;
  explicit TableSchema(const std::string& name);

  // ----- 属性 -----
  const std::string& name() const { return name_; }
  TableSchema& set_name(const std::string& name) {
    name_ = name;
    return *this;
  }

  const std::vector<ColumnDef>& columns() const { return columns_; }
  std::vector<ColumnDef>& columns() { return columns_; }

  // ----- 链式 API（返回 *this，内部记录错误）-----
  TableSchema& primary_key(const std::string& name, DataType type);
  TableSchema& not_null(const std::string& name, DataType type);
  TableSchema& nullable(const std::string& name, DataType type);
  TableSchema& add_column(const ColumnDef& col);
  TableSchema& add_column(const std::string& name, DataType type,
                          bool pk = false, bool nullable = true);

  // ----- 查询 -----
  int column_index(const std::string& name) const;
  const ColumnDef* column(const std::string& name) const;
  std::string primary_key_name() const;
  int primary_key_index() const { return primary_key_index_; }

  // ----- 验证 -----
  SchemaError validate() const;
  SchemaError validate_row(const Row& row) const;
  bool has_primary_key() const;
  bool has_error() const { return error_ != SchemaError::OK; }
  SchemaError error() const { return error_; }
  void clear_error() { error_ = SchemaError::OK; }

  // ----- 序列化 -----
  std::string serialize() const;
  static TableSchema deserialize(const std::string& data);

  // ----- 错误信息 -----
  static const char* error_message(SchemaError err);

 private:
  std::string name_;
  std::vector<ColumnDef> columns_;
  int primary_key_index_ = -1;
  SchemaError error_ = SchemaError::OK;

  // ----- 内部辅助 -----
  SchemaError check_duplicate_primary_key(const ColumnDef& col) const;
  SchemaError check_duplicate_column_name(const std::string& name) const;
  void set_error(SchemaError err) {
    if (error_ == SchemaError::OK) {
      error_ = err;  // 只记录第一个错误
    }
  }
};

}  // namespace sql