// row.h
#pragma once

#include "identifier.h"
#include <cstdint>
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

  // 显式声明拷贝/移动语义（确保正确）
  Row(const Row&) = default;
  Row(Row&&) = default;
  Row& operator=(const Row&) = default;
  Row& operator=(Row&&) = default;

  size_t size() const { return values_.size(); }
  const Value& operator[](size_t index) const { return values_[index]; }
  Value& operator[](size_t index) { return values_[index]; }

  bool is_empty() const { return values_.empty(); }
  auto begin() { return values_.begin(); }
  auto end() { return values_.end(); }
  auto begin() const { return values_.begin(); }
  auto end() const { return values_.end(); }
  auto rbegin() { return values_.rbegin(); }
  auto rend() { return values_.rend(); }
  auto rbegin() const { return values_.rbegin(); }
  auto rend() const { return values_.rend(); }

  std::string to_string() const;

  // 序列化：推荐用带 schema 的版本 —— NULL 会按所属列的 family 编码
  std::string serialize() const;
  std::string serialize(const TableSchema& schema) const;
  // 反序列化（需要 schema 才能解码每列的 key）
  static std::expected<Row, SchemaError> deserialize(
      const std::string& data, const TableSchema& schema);

  // 序列化格式版本；格式变化必须递增
  static constexpr uint8_t kFormatVersion = 1;

  bool operator==(const Row& other) const { return values_ == other.values_; }
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
  RowBuilder& set(const Identifier& column, int value) {
    values_[column] = Value(value);
    return *this;
  }

  RowBuilder& set(const Identifier& column, int64_t value) {
    values_[column] = Value(value);
    return *this;
  }

  // ✅ 字符串
  RowBuilder& set(const Identifier& column, const std::string& value) {
    values_[column] = Value(value);
    return *this;
  }

  // ✅ 字符串字面量
  RowBuilder& set(const Identifier& column, const char* value) {
    values_[column] = Value(std::string(value));
    return *this;
  }

  // ✅ 布尔值 - 单独方法避免歧义
  RowBuilder& set_bool(const Identifier& column, bool value) {
    values_[column] = Value(value);
    return *this;
  }

  // ✅ 显式 NULL
  RowBuilder& set_null(const Identifier& column) {
    values_[column] = Value();
    return *this;
  }

  // ✅ 通用 Value（高级用法）
  RowBuilder& set(const Identifier& column, const Value& value) {
    values_[column] = value;
    return *this;
  }

  std::expected<Row, SchemaError> build(const TableSchema& schema) const;
  std::expected<Row, SchemaError> build_ordered(
      const TableSchema& schema, const std::vector<Identifier>& order) const;

  Value get(const Identifier& column) const;
  bool has(const Identifier& column) const;

 private:
  std::unordered_map<Identifier, Value> values_;
};

inline RowBuilder row() { return RowBuilder(); }

}  // namespace sql
