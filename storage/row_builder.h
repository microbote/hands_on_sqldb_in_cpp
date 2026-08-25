// row_builder.h
#ifndef ROW_BUILDER_H
#define ROW_BUILDER_H

#include <initializer_list>
#include <string>
#include <unordered_map>
#include <vector>

#include "storage_engine.h"

namespace storage {

// ============================================================
// RowBuilder - 构建行数据的辅助类
// ============================================================
class RowBuilder {
 public:
  RowBuilder() = default;
  ~RowBuilder() = default;

  // ---- 链式设置 ----
  RowBuilder& set(const std::string& column, int64_t value) {
    values_[column] = Value(value);
    return *this;
  }

  RowBuilder& set(const std::string& column, const std::string& value) {
    values_[column] = Value(value);
    return *this;
  }

  RowBuilder& set(const std::string& column, const char* value) {
    values_[column] = Value(std::string(value));
    return *this;
  }

  RowBuilder& set(const std::string& column, bool value) {
    values_[column] = Value(value);
    return *this;
  }

  RowBuilder& set_null(const std::string& column) {
    values_[column] = Value();
    return *this;
  }

  // ---- 批量设置 ----
  RowBuilder& set_all(const std::unordered_map<std::string, Value>& values) {
    for (const auto& [col, val] : values) {
      values_[col] = val;
    }
    return *this;
  }

  // ---- 构建方法 ----
  // 按表结构顺序构建
  Row build(const TableSchema& schema) const {
    Row row;
    for (const auto& col : schema.columns) {
      auto it = values_.find(col.name);
      if (it != values_.end()) {
        row.push_back(it->second);
      } else {
        row.push_back(Value());  // NULL
      }
    }
    return row;
  }

  // 按指定顺序构建
  Row build_ordered(const std::vector<std::string>& column_order) const {
    Row row;
    for (const auto& col : column_order) {
      auto it = values_.find(col);
      if (it != values_.end()) {
        row.push_back(it->second);
      } else {
        row.push_back(Value());
      }
    }
    return row;
  }

  // 直接构建（不排序，按插入顺序）
  Row build() const {
    Row row;
    for (const auto& [col, val] : values_) {
      row.push_back(val);
    }
    return row;
  }

  // ---- 查询 ----
  bool has(const std::string& column) const {
    return values_.find(column) != values_.end();
  }

  Value get(const std::string& column) const {
    auto it = values_.find(column);
    return it != values_.end() ? it->second : Value();
  }

  size_t size() const { return values_.size(); }

  // ---- 清空 ----
  void clear() { values_.clear(); }

  // ---- 调试 ----
  std::string to_string() const {
    std::string result = "{";
    bool first = true;
    for (const auto& [col, val] : values_) {
      if (!first) result += ", ";
      result += col + "=" + val.to_string();
      first = false;
    }
    result += "}";
    return result;
  }

  // ---- 迭代器支持 ----
  auto begin() const { return values_.begin(); }
  auto end() const { return values_.end(); }

 private:
  std::unordered_map<std::string, Value> values_;
};

// ============================================================
// 便捷函数
// ============================================================
inline RowBuilder row() { return RowBuilder(); }

// ============================================================
// 从 initializer_list 创建 Row（用于简单场景）
// ============================================================
inline Row make_row(
    const TableSchema& schema,
    std::initializer_list<std::pair<const std::string, Value>> init) {
  RowBuilder builder;
  for (const auto& [col, val] : init) {
    builder.set(col, val);
  }
  return builder.build(schema);
}

// ============================================================
// 更新辅助
// ============================================================
class UpdateBuilder {
 public:
  UpdateBuilder() = default;
  ~UpdateBuilder() = default;

  UpdateBuilder& set(const std::string& column, int64_t value) {
    assignments_[column] = Value(value);
    return *this;
  }

  UpdateBuilder& set(const std::string& column, const std::string& value) {
    assignments_[column] = Value(value);
    return *this;
  }

  UpdateBuilder& set(const std::string& column, bool value) {
    assignments_[column] = Value(value);
    return *this;
  }

  std::vector<std::pair<std::string, Value>> build() const {
    std::vector<std::pair<std::string, Value>> result;
    for (const auto& [col, val] : assignments_) {
      result.push_back({col, val});
    }
    return result;
  }

 private:
  std::unordered_map<std::string, Value> assignments_;
};

inline UpdateBuilder update() { return UpdateBuilder(); }

}  // namespace storage

#endif  // ROW_BUILDER_H