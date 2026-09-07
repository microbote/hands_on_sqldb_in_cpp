// row.h
#pragma once

#include <string>
#include <unordered_map>
#include <vector>

#include "value.h"
#include <expected> 

namespace sql {

class TableSchema;
enum class SchemaError : uint8_t;

class Row {
 public:
  Row() = default;
  explicit Row(const std::vector<Value>& values) : values_(values) {}

  size_t size() const { return values_.size(); }
  const Value& operator[](size_t index) const { return values_[index]; }
  Value& operator[](size_t index) { return values_[index]; }

  auto begin() { return values_.begin(); }
  auto end() { return values_.end(); }
  auto begin() const { return values_.begin(); }
  auto end() const { return values_.end(); }

  std::string to_string() const;
  std::string serialize() const;
  static Row deserialize(const std::string& data, const TableSchema& schema);

  bool operator==(const Row& other) const;
  bool operator!=(const Row& other) const { return !(*this == other); }

  void push_back(const Value& value) { values_.push_back(value); }
  void reserve(size_t size) { values_.reserve(size); }
 private:
  std::vector<Value> values_;
};

// ============================================================
// 行构建器
// ============================================================
class RowBuilder {
 public:
  RowBuilder() = default;

  // ✅ 整数 - 使用 int，避免和 bool 冲突
  RowBuilder& set(const std::string& column, int value) {
    values_[column] = Value(static_cast<int64_t>(value));
    return *this;
  }

  // ✅ 字符串
  RowBuilder& set(const std::string& column, const std::string& value) {
    values_[column] = Value(value);
    return *this;
  }

  // ✅ 字符串字面量
  RowBuilder& set(const std::string& column, const char* value) {
    values_[column] = Value(std::string(value));
    return *this;
  }

  // ✅ 布尔值 - 单独方法避免歧义
  RowBuilder& set_bool(const std::string& column, bool value) {
    values_[column] = Value(value);
    return *this;
  }

  // ✅ 显式 NULL
  RowBuilder& set_null(const std::string& column) {
    values_[column] = Value();
    return *this;
  }

  // ✅ 通用 Value（高级用法）
  RowBuilder& set(const std::string& column, const Value& value) {
    values_[column] = value;
    return *this;
  }

  std::expected<Row, SchemaError> build(const TableSchema& schema) const;
  std::expected<Row, SchemaError> build_ordered(
      const TableSchema& schema, const std::vector<std::string>& order) const;

  Value get(const std::string& column) const;
  bool has(const std::string& column) const;

 private:
  std::unordered_map<std::string, Value> values_;
};

inline RowBuilder row() { return RowBuilder(); }

}  // namespace sql