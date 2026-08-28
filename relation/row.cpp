// row.cpp
#include "row.h"
#include "schema.h"

#include <sstream>

namespace sql {

std::string Row::to_string() const {
  std::ostringstream oss;
  oss << "[";
  for (size_t i = 0; i < values_.size(); ++i) {
    if (i > 0) oss << ", ";
    oss << values_[i].to_string();
  }
  oss << "]";
  return oss.str();
}

std::string Row::serialize() const {
  std::ostringstream oss;
  for (size_t i = 0; i < values_.size(); ++i) {
    if (i > 0) oss << "|";
    oss << values_[i].to_string();
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
      row.push_back(Value::from_string(val_str, col.type));
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
  if (values_.size() != other.values_.size()) return false;
  for (size_t i = 0; i < values_.size(); ++i) {
    if (values_[i] != other.values_[i]) return false;
  }
  return true;
}

// ============================================================
// RowBuilder 实现
// ============================================================

Row RowBuilder::build(const TableSchema& schema) const {
  Row row;
  for (const auto& col : schema.columns()) {
    auto it = values_.find(col.name);
    if (it != values_.end()) {
      row.push_back(it->second);
    } else {
      row.push_back(Value());  // NULL
    }
  }
  return row;
}

Row RowBuilder::build_ordered(const std::vector<std::string>& order) const {
  Row row;
  for (const auto& col : order) {
    auto it = values_.find(col);
    if (it != values_.end()) {
      row.push_back(it->second);
    } else {
      row.push_back(Value());
    }
  }
  return row;
}

Value RowBuilder::get(const std::string& column) const {
  auto it = values_.find(column);
  return it != values_.end() ? it->second : Value();
}

bool RowBuilder::has(const std::string& column) const {
  return values_.find(column) != values_.end();
}

}  // namespace sql