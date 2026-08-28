// schema.cpp
#include "schema.h"
#include "row.h"
#include <sstream>

namespace sql {

void TableSchema::add_column(const ColumnDef& col) {
  columns_.push_back(col);
  if (col.primary_key) {
    primary_key_index_ = static_cast<int>(columns_.size()) - 1;
  }
}

void TableSchema::add_column(const std::string& name, DataType type, bool pk,
                             bool nullable) {
  add_column(ColumnDef(name, type, pk, nullable));
}

int TableSchema::column_index(const std::string& name) const {
  for (size_t i = 0; i < columns_.size(); ++i) {
    if (columns_[i].name == name) {
      return static_cast<int>(i);
    }
  }
  return -1;
}

const ColumnDef* TableSchema::column(const std::string& name) const {
  int idx = column_index(name);
  if (idx >= 0 && idx < static_cast<int>(columns_.size())) {
    return &columns_[idx];
  }
  return nullptr;
}

std::string TableSchema::primary_key_name() const {
  if (primary_key_index_ >= 0 &&
      primary_key_index_ < static_cast<int>(columns_.size())) {
    return columns_[primary_key_index_].name;
  }
  return "";
}

bool TableSchema::validate_row(const Row& row) const {
  if (row.size() != columns_.size()) {
    return false;
  }

  for (size_t i = 0; i < columns_.size(); ++i) {
    if (row[i].is_null() && !columns_[i].nullable) {
      return false;
    }
    if(!row[i].is_null() && row[i].type() != columns_[i].type) {
      return false;
    }
  }

  return true;
}

std::string TableSchema::serialize() const {
  std::ostringstream oss;
  oss << name_ << "|";
  oss << primary_key_index_ << "|";

  for (size_t i = 0; i < columns_.size(); ++i) {
    if (i > 0) oss << ",";
    const auto& col = columns_[i];
    oss << col.name << ":";
    oss << static_cast<int>(col.type) << ":";
    oss << (col.nullable ? "1" : "0") << ":";
    oss << (col.primary_key ? "1" : "0");
  }

  return oss.str();
}

TableSchema TableSchema::deserialize(const std::string& data) {
  TableSchema schema;
  std::stringstream ss(data);
  std::string token;

  // 解析表名
  std::getline(ss, token, '|');
  schema.set_name(token);

  // 解析主键索引
  std::getline(ss, token, '|');
  if (!token.empty()) {
    schema.primary_key_index_ = std::stoi(token);
  }

  // 解析列
  while (std::getline(ss, token, ',')) {
    if (token.empty()) continue;

    std::stringstream col_ss(token);
    std::string part;
    ColumnDef col;

    std::getline(col_ss, part, ':');
    col.name = part;

    std::getline(col_ss, part, ':');
    col.type = static_cast<DataType>(std::stoi(part));

    std::getline(col_ss, part, ':');
    col.nullable = (part == "1");

    std::getline(col_ss, part, ':');
    col.primary_key = (part == "1");

    schema.columns_.push_back(col);

    if (col.primary_key) {
      schema.primary_key_index_ = static_cast<int>(schema.columns_.size()) - 1;
    }
  }

  return schema;
}

}  // namespace sql