// schema.cpp
#include "schema.h"
#include <sstream>
#include <iomanip>
#include <algorithm>
#include "row.h"

namespace sql {

// ============================================================
// 构造
// ============================================================
TableSchema::TableSchema(const Identifier& name) : name_(name) {}

// ============================================================
// 链式 API
// ============================================================

TableSchema& TableSchema::add_column(const ColumnDef& col) {
  // 如果已经有错误，跳过后续操作
  if (has_error()) {
    return *this;
  }

  // 1. 检查主键唯一性
  auto err = check_duplicate_primary_key(col);
  if (err != SchemaError::OK) {
    set_error(err);
    return *this;
  }

  // 2. 检查列名重复
  err = check_duplicate_column_name(col.name);
  if (err != SchemaError::OK) {
    set_error(err);
    return *this;
  }

  // 3. 添加列
  columns_.push_back(col);
  if (col.primary_key) {
    primary_key_index_ = static_cast<int>(columns_.size()) - 1;
  }

  return *this;
}

TableSchema& TableSchema::add_column(const Identifier& name, DataType type,
                                     bool pk, bool nullable) {
  return add_column(ColumnDef(name, type, pk, nullable));
}

TableSchema& TableSchema::primary_key(const Identifier& name, DataType type) {
  if (has_error()) {
    return *this;
  }

  if (has_primary_key()) {
    set_error(SchemaError::DUPLICATE_PRIMARY_KEY);
    return *this;
  }

  ColumnDef col(name, type, true, false);
  columns_.push_back(col);
  primary_key_index_ = static_cast<int>(columns_.size()) - 1;
  return *this;
}

TableSchema& TableSchema::not_null(const Identifier& name, DataType type) {
  return add_column(ColumnDef(name, type, false, false));
}

TableSchema& TableSchema::nullable(const Identifier& name, DataType type) {
  return add_column(ColumnDef(name, type, false, true));
}

// ============================================================
// 验证
// ============================================================

SchemaError TableSchema::validate() const {
  // 如果已经记录了错误，直接返回
  if (has_error()) {
    return error_;
  }

  // 1. 必须有主键
  if (!has_primary_key()) {
    return SchemaError::NO_PRIMARY_KEY;
  }

  // 2. 主键不能为 NULL
  if (columns_[primary_key_index_].nullable) {
    return SchemaError::INVALID_ROW;
  }

  std::vector<Identifier> primary_keys;
  // 3. 检查列名重复（这个已经在 add_column 中检查了，但再确认一次）
  for (size_t i = 0; i < columns_.size(); ++i) {
    if (columns_[i].primary_key){
      primary_keys.push_back(columns_[i].name);
    }
    for (size_t j = i + 1; j < columns_.size(); ++j) {
      if (columns_[i].name == columns_[j].name) {
        return SchemaError::DUPLICATE_COLUMN_NAME;
      }
    }
  }
  if(primary_keys.size()  > 1) {
    fprintf(stderr, "table:[%s] has more than one primary key:[%s,%s,...]\n", name_.c_str(), primary_keys[0].c_str(), primary_keys[1].c_str());
    return SchemaError::DUPLICATE_PRIMARY_KEY;
  }

  return SchemaError::OK;
}

SchemaError TableSchema::validate_row(const Row& row) const {
  if (row.size() != columns_.size()) {
    return SchemaError::COLUMN_SIZE_MISMATCH;
  }
  for (size_t i = 0; i < columns_.size(); ++i) {
    if (row[i].is_null() && !columns_[i].nullable) {
      return SchemaError::COLUMN_ATTR_NULL_MISMATCH;
    }
    if (!row[i].is_null() && columns_[i].type != row[i].type()) {
      return SchemaError::COLUMN_TYPE_MISMATCH;
    }
  }
  return SchemaError::OK;
}

bool TableSchema::has_primary_key() const {
  return primary_key_index_ >= 0 &&
         primary_key_index_ < static_cast<int>(columns_.size());
}

// ============================================================
// 查询
// ============================================================

int TableSchema::column_index(const Identifier& name) const {
  for (size_t i = 0; i < columns_.size(); ++i) {
    if (columns_[i].name == name) {
      return static_cast<int>(i);
    }
  }
  return -1;
}

const ColumnDef* TableSchema::column(const Identifier& name) const {
  int idx = column_index(name);
  return idx >= 0 ? &columns_[idx] : nullptr;
}

Identifier TableSchema::primary_key_name() const {
  if (has_primary_key()) {
    return columns_[primary_key_index_].name;
  }
  return {};
}

// ============================================================
// 序列化
// ============================================================

std::string TableSchema::serialize() const {
  std::ostringstream oss;
  oss << name_ << "|";
  oss << primary_key_index_ << "|";
  for (size_t i = 0; i < columns_.size(); ++i) {
    if (i > 0) oss << ",";
    const auto& col = columns_[i];
    oss << col.name << ":" << static_cast<int>(col.type) << ":"
        << (col.nullable ? "1" : "0") << ":" << (col.primary_key ? "1" : "0");
  }
  return oss.str();
}

TableSchema TableSchema::deserialize(const std::string& data) {
  TableSchema schema;
  std::stringstream ss(data);
  std::string token;

  std::getline(ss, token, '|');
  schema.set_name(Identifier(token));

  std::getline(ss, token, '|');
  if (!token.empty()) {
    schema.primary_key_index_ = std::stoi(token);
  }

  while (std::getline(ss, token, ',')) {
    if (token.empty()) continue;
    std::stringstream col_ss(token);
    std::string part;
    ColumnDef col;

    std::getline(col_ss, part, ':');
    col.name = identifier(part);

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

// ============================================================
// 错误信息
// ============================================================

const char* TableSchema::error_message(SchemaError err) {
  switch (err) {
    case SchemaError::OK:
      return "OK";
    case SchemaError::DUPLICATE_PRIMARY_KEY:
      return "Duplicate primary key: table already has a primary key";
    case SchemaError::NO_PRIMARY_KEY:
      return "No primary key defined";
    case SchemaError::DUPLICATE_COLUMN_NAME:
      return "Duplicate column name";
    case SchemaError::COLUMN_SIZE_MISMATCH:
      return "Row size mismatch";
    case SchemaError::COLUMN_TYPE_MISMATCH:
      return "Type mismatch";
    case SchemaError::COLUMN_ATTR_NULL_MISMATCH:
      return "NULL constraint violation";
    case SchemaError::COLUMN_NOT_FOUND:
      return "Column not found";
    default:
      return "Unknown error";
  }
}

// ============================================================
// 内部辅助
// ============================================================

SchemaError TableSchema::check_duplicate_primary_key(
    const ColumnDef& col) const {
  if (col.primary_key && has_primary_key()) {
    return SchemaError::DUPLICATE_PRIMARY_KEY;
  }
  return SchemaError::OK;
}

SchemaError TableSchema::check_duplicate_column_name(
    const std::string& name) const {
  for (const auto& existing : columns_) {
    if (existing.name == name) {
      return SchemaError::DUPLICATE_COLUMN_NAME;
    }
  }
  return SchemaError::OK;
}

// ============================================================
// to_string - 单行紧凑格式
// ============================================================
std::string TableSchema::to_string() const {
  std::ostringstream oss;
  oss << name_.str() << "(";
  
  for (size_t i = 0; i < columns_.size(); ++i) {
    if (i > 0) { oss << ", ";
}
    const auto& col = columns_[i];
    oss << col.name.str() << " " << data_type_name(col.type);
    if (col.primary_key) { oss << " PRIMARY KEY";
    } else if (!col.nullable) { oss << " NOT NULL";
}
  }
  
  oss << ")";
  return oss.str();
}

// ============================================================
// to_string_pretty - 多行对齐格式
// ============================================================
std::string TableSchema::to_string_pretty() const {
  if (columns_.empty()) {
    return name_.str() + " (empty table)";
  }
  
  std::ostringstream oss;
  oss << "Table: " << name_.str() << "\n";
  oss << "Columns:\n";
  
  // 表头
  oss << "  " << std::left << std::setw(25) << "Name" 
      << std::setw(15) << "Type" 
      << std::setw(10) << "Nullable" 
      << "Constraint\n";
  oss << "  " << std::string(50, '-') << "\n";
  
  for (const auto& col : columns_) {
    std::string nullable = col.nullable ? "YES" : "NO";
    std::string constraint;
    if (col.primary_key) {
      constraint = "PRIMARY KEY";
    } else if (!col.nullable) {
      constraint = "NOT NULL";
    }
    
    oss << "  " << std::left << std::setw(25) << col.name.str()
        << std::setw(15) << data_type_name(col.type)
        << std::setw(10) << nullable
        << constraint << "\n";
  }
  
  return oss.str();
}

// ============================================================
// to_string_table - 类似 psql 的表格形式
// ============================================================
std::string TableSchema::to_string_table() const {
  if (columns_.empty()) {
    return "Table \"" + name_.str() + "\" has no columns.\n";
  }
  
  // 计算列宽
  int name_width = max_column_name_width() + 2;
  int type_width = max_type_width() + 2;
  int null_width = 8;
  
  std::ostringstream oss;
  oss << "Table \"" << name_.str() << "\"\n";
  
  // 上边框
  oss << "+" << std::string(name_width + 2, '-')
      << "+" << std::string(type_width + 2, '-')
      << "+" << std::string(null_width + 2, '-')
      << "+" << std::string(12 + 2, '-') << "+\n";
  
  // 表头
  oss << "| " << std::left << std::setw(name_width) << "Column"
      << "| " << std::setw(type_width) << "Type"
      << "| " << std::setw(null_width) << "Nullable"
      << "| " << std::setw(12) << "Constraint" << "|\n";
  
  // 表头分隔线
  oss << "|" << std::string(name_width + 2, '-')
      << "|" << std::string(type_width + 2, '-')
      << "|" << std::string(null_width + 2, '-')
      << "|" << std::string(12 + 2, '-') << "|\n";
  
  // 数据行
  for (const auto& col : columns_) {
    std::string nullable = col.nullable ? "YES" : "NO";
    std::string constraint;
    if (col.primary_key) {
      constraint = "PRIMARY KEY";
    } else if (!col.nullable) {
      constraint = "NOT NULL";
    }
    
    oss << "| " << std::left << std::setw(name_width) << col.name.str()
        << "| " << std::setw(type_width) << data_type_name(col.type)
        << "| " << std::setw(null_width) << nullable
        << "| " << std::setw(12) << constraint << "|\n";
  }
  
  // 下边框
  oss << "+" << std::string(name_width + 2, '-')
      << "+" << std::string(type_width + 2, '-')
      << "+" << std::string(null_width + 2, '-')
      << "+" << std::string(12 + 2, '-') << "+\n";
  
  return oss.str();
}

// ============================================================
// to_string_summary - 简短摘要
// ============================================================
std::string TableSchema::to_string_summary() const {
  std::ostringstream oss;
  oss << name_.str() << "(" << columns_.size() << " columns";
  if (has_primary_key()) {
    oss << ", PK: " << primary_key_column()->name.str();
  }
  oss << ")";
  return oss.str();
}

// ============================================================
// 相等比较
// ============================================================
bool TableSchema::operator==(const TableSchema& other) const {
  return name_ == other.name_ && 
         columns_ == other.columns_ &&
         primary_key_index_ == other.primary_key_index_;
}

// ============================================================
// 辅助函数
// ============================================================
int TableSchema::max_column_name_width() const {
  int max_width = 0;
  for (const auto& col : columns_) {
    max_width = std::max(max_width, (int)col.name.str().length());
  }
  return max_width;
}

int TableSchema::max_type_width() const {
  int max_width = 0;
  for (const auto& col : columns_) {
    max_width = std::max(max_width, 
                         (int)std::string(data_type_name(col.type)).length());
  }
  return max_width;
}

}  // namespace sql