// condition.cpp
#include "condition.h"

#include <algorithm>
#include <iomanip>
#include <sstream>

namespace sql {

// ============================================================
// 构造
// ============================================================
ConditionExpr::ConditionExpr(const std::string& col, CompareOp op,
                             const storage::Value& val)
    : type_(ConditionType::COMPARE), column_(col), op_(op), value_(val) {}

ConditionExpr::ConditionExpr(ConditionType type,
                             std::unique_ptr<ConditionExpr> left,
                             std::unique_ptr<ConditionExpr> right)
    : type_(type), left_(std::move(left)), right_(std::move(right)) {}

ConditionExpr::ConditionExpr(std::unique_ptr<ConditionExpr> child)
    : type_(ConditionType::NOT), left_(std::move(child)), right_(nullptr) {}

// 拷贝构造
ConditionExpr::ConditionExpr(const ConditionExpr& other)
    : type_(other.type_),
      column_(other.column_),
      op_(other.op_),
      value_(other.value_) {
  if (other.left_) {
    left_ = std::make_unique<ConditionExpr>(*other.left_);
  }
  if (other.right_) {
    right_ = std::make_unique<ConditionExpr>(*other.right_);
  }
}

ConditionExpr::ConditionExpr(ConditionExpr&& other) noexcept
    : type_(other.type_),
      column_(std::move(other.column_)),
      op_(other.op_),
      value_(std::move(other.value_)),
      left_(std::move(other.left_)),
      right_(std::move(other.right_)) {}

ConditionExpr& ConditionExpr::operator=(const ConditionExpr& other) {
  if (this != &other) {
    type_ = other.type_;
    column_ = other.column_;
    op_ = other.op_;
    value_ = other.value_;
    left_ =
        other.left_ ? std::make_unique<ConditionExpr>(*other.left_) : nullptr;
    right_ =
        other.right_ ? std::make_unique<ConditionExpr>(*other.right_) : nullptr;
  }
  return *this;
}

ConditionExpr& ConditionExpr::operator=(ConditionExpr&& other) noexcept {
  if (this != &other) {
    type_ = other.type_;
    column_ = std::move(other.column_);
    op_ = other.op_;
    value_ = std::move(other.value_);
    left_ = std::move(other.left_);
    right_ = std::move(other.right_);
  }
  return *this;
}

// ============================================================
// 匹配
// ============================================================
bool ConditionExpr::matches(const storage::Row& row,
                            const storage::TableSchema& schema) const {
  switch (type_) {
    case ConditionType::COMPARE:
      return matches_compare(row, schema);
    case ConditionType::AND:
      return left_ && right_ && left_->matches(row, schema) &&
             right_->matches(row, schema);
    case ConditionType::OR:
      return left_ && right_ &&
             (left_->matches(row, schema) || right_->matches(row, schema));
    case ConditionType::NOT:
      return left_ && !left_->matches(row, schema);
  }
  return false;
}

bool ConditionExpr::matches_compare(const storage::Row& row,
                                    const storage::TableSchema& schema) const {
  int idx = schema.get_column_index(column_);
  if (idx < 0 || idx >= static_cast<int>(row.size())) {
    return false;
  }
  return compare_values(row[idx]);
}

bool ConditionExpr::compare_values(const storage::Value& row_val) const {
  if (row_val.type == storage::DataType::NULL_TYPE) {
    return op_ == CompareOp::NE;
  }

  switch (op_) {
    case CompareOp::EQ:
      return row_val == value_;
    case CompareOp::NE:
      return row_val != value_;
    case CompareOp::GT:
      if (row_val.type != storage::DataType::INTEGER ||
          value_.type != storage::DataType::INTEGER)
        return false;
      return row_val.int_val > value_.int_val;
    case CompareOp::GE:
      if (row_val.type != storage::DataType::INTEGER ||
          value_.type != storage::DataType::INTEGER)
        return false;
      return row_val.int_val >= value_.int_val;
    case CompareOp::LT:
      if (row_val.type != storage::DataType::INTEGER ||
          value_.type != storage::DataType::INTEGER)
        return false;
      return row_val.int_val < value_.int_val;
    case CompareOp::LE:
      if (row_val.type != storage::DataType::INTEGER ||
          value_.type != storage::DataType::INTEGER)
        return false;
      return row_val.int_val <= value_.int_val;
    case CompareOp::LIKE:
      if (row_val.type != storage::DataType::STRING ||
          value_.type != storage::DataType::STRING)
        return false;
      return like_match(row_val.str_val, value_.str_val);
    default:
      return false;
  }
}

bool ConditionExpr::like_match(const std::string& str,
                               const std::string& pattern) const {
  if (pattern == "*" || pattern == "%") return true;
  if (pattern.empty()) return str.empty();

  // 支持 * 和 % 通配符
  std::string p = pattern;
  std::replace(p.begin(), p.end(), '%', '*');

  size_t pos = p.find('*');
  if (pos == std::string::npos) {
    return str == p;
  }

  if (pos == 0) {
    // *suffix
    std::string suffix = p.substr(1);
    if (str.length() < suffix.length()) return false;
    return str.substr(str.length() - suffix.length()) == suffix;
  } else if (pos == p.length() - 1) {
    // prefix*
    std::string prefix = p.substr(0, pos);
    return str.substr(0, prefix.length()) == prefix;
  } else {
    // prefix*suffix
    std::string prefix = p.substr(0, pos);
    std::string suffix = p.substr(pos + 1);
    if (str.length() < prefix.length() + suffix.length()) return false;
    return str.substr(0, prefix.length()) == prefix &&
           str.substr(str.length() - suffix.length()) == suffix;
  }
}

// ============================================================
// 主键条件提取
// ============================================================
bool ConditionExpr::only_primary_key(const std::string& pk_column) const {
  if (type_ == ConditionType::COMPARE) {
    return column_ == pk_column;
  }

  if (type_ == ConditionType::AND || type_ == ConditionType::OR) {
    return left_ && right_ && left_->only_primary_key(pk_column) &&
           right_->only_primary_key(pk_column);
  }

  if (type_ == ConditionType::NOT) {
    return left_ && left_->only_primary_key(pk_column);
  }

  return false;
}

void ConditionExpr::collect_primary_key_conditions(
    const std::string& pk_column,
    std::vector<std::pair<CompareOp, storage::Value>>& result) const {
  if (type_ == ConditionType::COMPARE) {
    if (column_ == pk_column) {
      result.push_back({op_, value_});
    }
    return;
  }

  if (type_ == ConditionType::AND) {
    if (left_) left_->collect_primary_key_conditions(pk_column, result);
    if (right_) right_->collect_primary_key_conditions(pk_column, result);
    return;
  }

  // OR 和 NOT 比较复杂，暂不处理
  if (type_ == ConditionType::OR || type_ == ConditionType::NOT) {
    // 对于 OR，需要收集所有条件
    if (left_) left_->collect_primary_key_conditions(pk_column, result);
    if (right_) right_->collect_primary_key_conditions(pk_column, result);
  }
}

bool ConditionExpr::extract_primary_key_conditions(
    const std::string& pk_column,
    std::vector<std::pair<CompareOp, storage::Value>>& conditions) const {
  conditions.clear();
  collect_primary_key_conditions(pk_column, conditions);
  return !conditions.empty();
}

// ============================================================
// 字符串转换
// ============================================================
std::string ConditionExpr::to_string() const {
  switch (type_) {
    case ConditionType::COMPARE:
      return column_ + " " + compare_op_to_string(op_) + " " +
             value_.to_string();
    case ConditionType::AND:
      return "(" + left_->to_string() + " AND " + right_->to_string() + ")";
    case ConditionType::OR:
      return "(" + left_->to_string() + " OR " + right_->to_string() + ")";
    case ConditionType::NOT:
      return "NOT (" + left_->to_string() + ")";
  }
  return "";
}

std::string compare_op_to_string(CompareOp op) {
  switch (op) {
    case CompareOp::EQ:
      return "=";
    case CompareOp::NE:
      return "!=";
    case CompareOp::GT:
      return ">";
    case CompareOp::GE:
      return ">=";
    case CompareOp::LT:
      return "<";
    case CompareOp::LE:
      return "<=";
    case CompareOp::LIKE:
      return "LIKE";
  }
  return "?";
}

CompareOp string_to_compare_op(const std::string& str) {
  if (str == "=") return CompareOp::EQ;
  if (str == "!=") return CompareOp::NE;
  if (str == ">") return CompareOp::GT;
  if (str == ">=") return CompareOp::GE;
  if (str == "<") return CompareOp::LT;
  if (str == "<=") return CompareOp::LE;
  if (str == "LIKE") return CompareOp::LIKE;
  return CompareOp::EQ;
}

}  // namespace sql