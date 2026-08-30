// condition.cpp
#include "condition.h"

#include <sstream>

namespace query {

// ============================================================
// 构造
// ============================================================

ConditionExpr::ConditionExpr(ConditionType type, const std::string& column, CompareOp op,
                             const sql::Value& value)
    : type_(type), column_(column), op_(op), value_(value) {}

ConditionExpr::ConditionExpr(ConditionType type,
                             std::unique_ptr<ConditionExpr> left,
                             std::unique_ptr<ConditionExpr> right)
    : type_(type), left_(std::move(left)), right_(std::move(right)) {
  // AND/OR 需要两个子节点
}

ConditionExpr::ConditionExpr(ConditionType type,std::unique_ptr<ConditionExpr> child)
    : type_(type), left_(std::move(child)) {}

ConditionExpr::ConditionExpr(ConditionType type, const std::string& column,
                             std::vector<sql::Value> values)
    : type_(type), column_(column), in_values_(std::move(values)) {
      if(type_ == ConditionType::IN) {
        op_ = CompareOp::IN;
      }
    }

// ----- 拷贝 -----
ConditionExpr::ConditionExpr(const ConditionExpr& other)
    : type_(other.type_),
      column_(other.column_),
      op_(other.op_),
      value_(other.value_),
      in_values_(other.in_values_) {
  if (other.left_) {
    left_ = std::make_unique<ConditionExpr>(*other.left_);
  }
  if (other.right_) {
    right_ = std::make_unique<ConditionExpr>(*other.right_);
  }
}

// ----- 移动 -----
ConditionExpr::ConditionExpr(ConditionExpr&& other) noexcept
    : type_(other.type_),
      column_(std::move(other.column_)),
      op_(other.op_),
      value_(std::move(other.value_)),
      in_values_(std::move(other.in_values_)),
      left_(std::move(other.left_)),
      right_(std::move(other.right_)) {}

ConditionExpr& ConditionExpr::operator=(const ConditionExpr& other) {
  if (this != &other) {
    type_ = other.type_;
    column_ = other.column_;
    op_ = other.op_;
    value_ = other.value_;
    in_values_ = other.in_values_;
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
    in_values_ = std::move(other.in_values_);
    left_ = std::move(other.left_);
    right_ = std::move(other.right_);
  }
  return *this;
}

// ============================================================
// matches - 判断行是否匹配条件
// ============================================================

bool ConditionExpr::matches(const sql::Row& row,
                            const sql::TableSchema& schema) const {
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
    case ConditionType::IN:
      return matches_compare(row, schema);
    default:
      return false;
  }
}

bool ConditionExpr::matches_compare(const sql::Row& row,
                                    const sql::TableSchema& schema) const {
  int idx = schema.column_index(column_);
  if (idx < 0 || idx >= static_cast<int>(row.size())) {
    return false;
  }
  return compare_values(row[idx]);

}

bool ConditionExpr::compare_in_values(const sql::Value& row_val) const {
  if (row_val.is_null() || in_values_.empty()) {
    return false;
  }
  if(row_val.type() != in_values_[0].type()) return false;

  for(const auto& in_value : in_values_){
    if(row_val == in_value){
      return true;
    }
  }
  return false;
}

bool ConditionExpr::compare_values(const sql::Value& row_val) const {
  if (row_val.is_null()) {
    return op_ == CompareOp::NE;
  }

  switch (op_) {
    case CompareOp::EQ:
      return row_val == value_;
    case CompareOp::NE:
      return row_val != value_;
    case CompareOp::GT:
      if (!row_val.is_int() || !value_.is_int()) return false;
      return row_val.int_val() > value_.int_val();
    case CompareOp::GE:
      if (!row_val.is_int() || !value_.is_int()) return false;
      return row_val.int_val() >= value_.int_val();
    case CompareOp::LT:
      if (!row_val.is_int() || !value_.is_int()) return false;
      return row_val.int_val() < value_.int_val();
    case CompareOp::LE:
      if (!row_val.is_int() || !value_.is_int()) return false;
      return row_val.int_val() <= value_.int_val();
    case CompareOp::LIKE:
      if (!row_val.is_string() || !value_.is_string()) return false;
      return like_match(row_val.str_val(), value_.str_val());
    case CompareOp::IN:
      return compare_in_values(row_val);
    default:
      return false;
  }
}

bool ConditionExpr::like_match(const std::string& str,
                               const std::string& pattern) const {
  // 支持 % 和 * 通配符（% 为 SQL 标准，* 为兼容）
  std::string p = pattern;
  // 将 SQL 的 % 转换为内部通配符
  // 简单实现：支持前缀、后缀、包含匹配
  if (p == "%" || p == "*") return true;
  if (p.empty()) return str.empty();

  // 检查是否为 %suffix%
  if (p.front() == '%' && p.back() == '%') {
    std::string mid = p.substr(1, p.length() - 2);
    return str.find(mid) != std::string::npos;
  }
  // 检查是否为 prefix%
  if (p.back() == '%') {
    std::string prefix = p.substr(0, p.length() - 1);
    return str.find(prefix) == 0;
  }
  // 检查是否为 %suffix
  if (p.front() == '%') {
    std::string suffix = p.substr(1);
    if (str.length() < suffix.length()) return false;
    return str.substr(str.length() - suffix.length()) == suffix;
  }
  // 无通配符，精确匹配
  return str == p;
}

// ============================================================
// extract_pk_conditions - 提取主键条件（用于优化）
// ============================================================

bool ConditionExpr::extract_primary_key_conditions(
    const sql::TableSchema& schema,
    std::vector<std::pair<CompareOp, sql::Value>>& pk_conds,
    std::unique_ptr<ConditionExpr>& remaining) const {
  pk_conds.clear();
  remaining.reset();

  if (type_ == ConditionType::COMPARE) {
    if (column_ == schema.primary_key_name()) {
      pk_conds.push_back({op_, value_});
      return true;
    }else{
      // 非主键条件，保留
      remaining = std::make_unique<ConditionExpr>(*this);
      return false;
    }
  }
  else if (type_ == ConditionType::IN ) {
    if (column_ == schema.primary_key_name()){
      // IN 条件可以转换为多个 EQ 条件
      for (const auto& val : in_values_) {
        pk_conds.push_back({CompareOp::EQ, val});
      }
      return true;
    }else{
      // 非主键条件，保留
      remaining = std::make_unique<ConditionExpr>(*this);
      return false;
    }
  }

  if (type_ == ConditionType::AND) {
    std::unique_ptr<ConditionExpr> left_rem, right_rem;
    bool left_is_pk =
        left_ && left_->extract_primary_key_conditions(schema, pk_conds, left_rem);
    bool right_is_pk =
        right_ && right_->extract_primary_key_conditions(schema, pk_conds, right_rem);

    // 如果有剩余条件，组合成 AND
    if (left_rem && right_rem) {
      remaining = std::make_unique<ConditionExpr>(
          ConditionType::AND, std::move(left_rem), std::move(right_rem));
    } else if (left_rem) {
      remaining = std::move(left_rem);
    } else if (right_rem) {
      remaining = std::move(right_rem);
    }

    return left_is_pk || right_is_pk;
  }

  // OR/NOT 暂不支持提取主键优化
  remaining = std::make_unique<ConditionExpr>(*this);
  return false;
}

// ============================================================
// only_primary_key - 检查是否只涉及主键
// ============================================================

bool ConditionExpr::only_primary_key(const sql::TableSchema& schema) const {
  if (type_ == ConditionType::COMPARE) {
    return column_ == schema.primary_key_name();
  }
  else if (type_ == ConditionType::IN) {
    return column_ == schema.primary_key_name();
  }
  else if (type_ == ConditionType::AND || type_ == ConditionType::OR) {
    return left_ && right_ && left_->only_primary_key(schema) &&
           right_->only_primary_key(schema);
  }
  else if (type_ == ConditionType::NOT) {
    return left_ && left_->only_primary_key(schema);
  }
  else {
    return false;
  }

}

// ============================================================
// collect_pk_conditions - 内部收集主键条件
// ============================================================

void ConditionExpr::collect_pk_conditions(
    const sql::TableSchema& schema,
    std::vector<std::pair<CompareOp, sql::Value>>& result) const {
  if (type_ == ConditionType::COMPARE) {
    if (column_ == schema.primary_key_name()) {
      result.push_back({op_, value_});
    }
    return;
  }

  if (type_ == ConditionType::AND) {
    if (left_) left_->collect_pk_conditions(schema, result);
    if (right_) right_->collect_pk_conditions(schema, result);
    return;
  }

  // OR/NOT 比较复杂，暂不处理
  if (type_ == ConditionType::OR || type_ == ConditionType::NOT) {
    if (left_) left_->collect_pk_conditions(schema, result);
    if (right_) right_->collect_pk_conditions(schema, result);
  }
}

// ============================================================
// to_string - 调试输出
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
    case ConditionType::IN: {
      std::string result = column_ + " IN (";
      for (size_t i = 0; i < in_values_.size(); ++i) {
        if (i > 0) result += ", ";
        result += in_values_[i].to_string();
      }
      result += ")";
      return result;
    }
    default:
      return "?";
  }
}

// ============================================================
// 辅助函数
// ============================================================

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
    default:
      return "?";
  }
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

}  // namespace query