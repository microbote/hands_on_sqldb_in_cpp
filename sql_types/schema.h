// schema.h
#pragma once

#include <cstdint>
#include <expected>
#include <stdexcept>
#include <string>
#include <vector>


#include "identifier.h"
#include "field_type.h"

namespace sql {

class Row;
class Value;

// ============================================================
// 列定义
// ============================================================
struct ColumnDef {
  Identifier name;
  DataType type;
  bool nullable = true;
  bool primary_key = false;

  ColumnDef() = default;
  ColumnDef(const Identifier& n, DataType t, bool pk = false, bool nul = true)
      : name(n), type(t), nullable(nul), primary_key(pk) {}

  std::string to_string() const {
    std::string s = name.str() + " " + data_type_name(type);
    if (primary_key) s += " PRIMARY KEY";
    if (!nullable) s += " NOT NULL";
    return s;
  }

  bool operator==(const ColumnDef& other) const {
    return name == other.name && type == other.type && 
           nullable == other.nullable && primary_key == other.primary_key;
  }

  bool operator!=(const ColumnDef& other) const {
    return !(*this == other);
  }
};


// ============================================================
// 错误码
// ============================================================
enum class SchemaError :uint8_t {
  OK = 0,
  DUPLICATE_PRIMARY_KEY,
  NO_PRIMARY_KEY,
  DUPLICATE_COLUMN_NAME,
  INVALID_ROW,
  INVALID_FORMAT,          // 序列化数据损坏 / 版本不匹配
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
  // 序列化格式版本；格式变化时必须递增，反序列化会校验
  static constexpr uint8_t kFormatVersion = 1;

  TableSchema() = default;
  explicit TableSchema(const Identifier& name);

  // ----- 属性 -----
  const std::string& table_name_str() const { return name_.str(); }
  const Identifier& table_name() const { return name_; }
  TableSchema& set_name(const Identifier& name) {
    name_ = name;
    return *this;
  }
  std::vector<std::string> column_names() const {
    std::vector<std::string> names;
    for(auto col : columns_){
      names.push_back(col.name.display_name());
    }
    return names;
  }

  const std::vector<ColumnDef>& columns() const { return columns_; }
  std::vector<ColumnDef>& columns() { return columns_; }
  size_t column_count() const { return columns_.size(); }
  // ----- 链式 API（返回 *this，内部记录错误）-----
  TableSchema& primary_key(const Identifier& name, DataType type);
  TableSchema& not_null(const Identifier& name, DataType type);
  TableSchema& nullable(const Identifier& name, DataType type);
  TableSchema& add_column(const ColumnDef& col);
  TableSchema& add_column(const Identifier& name, DataType type,
                          bool pk = false, bool nullable = true);

  // ----- 查询 -----
  int column_index(const Identifier& name) const;
  const ColumnDef* column(const Identifier& name) const;
  const ColumnDef* column_at(int idx) const {
    if (idx < 0 || static_cast<size_t>(idx) >= columns_.size()) {
      throw std::out_of_range("TableSchema::column_at: index " +
                              std::to_string(idx) + " out of range (size " +
                              std::to_string(columns_.size()) + ")");
    }
    return &columns_[static_cast<size_t>(idx)];
  }
  DataType column_type(const Identifier& name) const {
    auto col = column(name);
    if(col) {
      return col->type;
    }
    return DataType::UNKNOWN_TYPE;
  }
  Identifier primary_key_name() const;
  int primary_key_index() const { return primary_key_index_; }
  const ColumnDef* primary_key_column() const {
    if(has_primary_key()){
      return column_at(primary_key_index());
    }
    return nullptr;
  }
  bool is_empty() const { return columns_.empty(); }
  // ----- 验证 -----
  SchemaError validate() const;
  SchemaError validate_row(const Row& row) const;
  // 按列定义校验单个值（类型族 + NOT NULL）
  SchemaError validate_value(const ColumnDef& col, const Value& value) const;
  SchemaError validate_value(const Identifier& column_name,
                             const Value& value) const;
  bool has_primary_key() const;
  bool has_error() const { return error_ != SchemaError::OK; }
  SchemaError error() const { return error_; }
  void clear_error() { error_ = SchemaError::OK; }

  // ----- 序列化 -----
  std::string serialize() const;
  static std::expected<TableSchema, SchemaError> deserialize(
      const std::string& data);

  // 单行紧凑格式: users(id INT PRIMARY KEY, name VARCHAR NOT NULL, age INT NULL)
  std::string to_string() const;
  
  // 多行友好格式（对齐列）
  std::string to_string_pretty() const;
  
  // 表格形式 (类似 psql 输出)
  std::string to_string_table() const;
  
  // 简短摘要: users(3 columns, PK: id)
  std::string to_string_summary() const;

  // ----- 相等比较 -----
  bool operator==(const TableSchema& other) const;
  bool operator!=(const TableSchema& other) const { return !(*this == other); }


  // ----- 错误信息 -----
  static const char* error_message(SchemaError err);

 private:
  Identifier name_;
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

  // 计算对齐的辅助函数
  int max_column_name_width() const;
  int max_type_width() const;
};

}  // namespace sql
