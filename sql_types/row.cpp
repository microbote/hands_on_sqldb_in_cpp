// row.cpp
#include "row.h"
#include "schema.h"

#include <sql_types/identifier.h>
#include <sstream>

namespace sql {

std::string Row::to_string() const {
  std::ostringstream oss;
  oss << "[";
  for (size_t i = 0; i < values_.size(); ++i) {
    if (i > 0) { oss << ", ";
}
    oss << values_[i].to_string();
  }
  oss << "]";
  return oss.str();
}

std::string Row::serialize() const {
  std::ostringstream oss;
  for (size_t i = 0; i < values_.size(); ++i) {
    if (i > 0) { oss << "|";
}
    oss << values_[i].to_key();
  }
  return oss.str();
}

Row Row::deserialize(const std::string& data, const TableSchema& schema) {
  Row row;
  std::stringstream ss(data);
  std::string val_str;
  size_t col_idx = 0;

  while (std::getline(ss, val_str, '|')) {
    if (col_idx < schema.columns().size()) {
      const auto& col = schema.columns()[col_idx];
      row.push_back(Value::from_key(val_str, col.type));
    } else {
      row.push_back(Value());
    }
    col_idx++;
  }

  // 补齐缺失的列
  while (row.size() < schema.columns().size()) {
    row.push_back(Value());
  }

  return row;
}

bool Row::operator==(const Row& other) const {
  if (values_.size() != other.values_.size()) { return false;
}
  for (size_t i = 0; i < values_.size(); ++i) {
    if (values_[i] != other.values_[i]) { return false;
}
  }
  return true;
}

// ============================================================
// RowBuilder 实现
// ============================================================

std::expected<Row, SchemaError> RowBuilder::build(
    const TableSchema& schema) const {
  Row row;
  row.reserve(schema.columns().size());

  for (const auto& col : schema.columns()) {
    auto it = values_.find(col.name);
    if (it != values_.end()) {
      row.push_back(it->second);
    } else {
      // ✅ 检查 NOT NULL 约束：缺失的列如果 NOT NULL，返回错误
      if (!col.nullable) {
        fprintf(stderr, "column:[%s] is not nullable\n", col.name.c_str());
        return std::unexpected(SchemaError::COLUMN_ATTR_NULL_MISMATCH);
      }
      row.push_back(Value());  // NULL
    }
  }

  // ✅ 验证行（类型检查、NULL 约束等）
  auto err = schema.validate_row(row);
  if (err != SchemaError::OK) {
    return std::unexpected(err);
  }

  return row;
}

std::expected<Row, SchemaError> RowBuilder::build_ordered(
    const TableSchema& schema, const std::vector<Identifier>& order) const {
  Row row;
  row.reserve(order.size());

  for (const auto& col_name : order) {
    auto it = values_.find(col_name);
    if (it != values_.end()) {
      row.push_back(it->second);
    } else {
      const auto* col = schema.column(col_name);
      if (!col) {
        return std::unexpected(SchemaError::COLUMN_NOT_FOUND);
      }
      if (!col->nullable) {
        return std::unexpected(SchemaError::COLUMN_ATTR_NULL_MISMATCH);
      }
      row.push_back(Value());
    }
  }

  auto err = schema.validate_row(row);
  if (err != SchemaError::OK) {
    return std::unexpected(err);
  }

  return row;
}

Value RowBuilder::get(const Identifier& column) const {
  auto it = values_.find(column);
  return it != values_.end() ? it->second : Value();
}

bool RowBuilder::has(const Identifier& column) const {
  return values_.find(column) != values_.end();
}

}  // namespace sql