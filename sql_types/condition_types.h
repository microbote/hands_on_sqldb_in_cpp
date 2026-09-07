// condition_types.h
#pragma once

#include <memory>
#include <string>
#include <vector>

#include "condition.h"
#include "condition_visitor.h"
#include "compare_op.h"
#include "value.h"

namespace sql {

// ============================================================
// CompareCondition：单值比较
// ============================================================
class CompareCondition : public Condition {
 public:
  CompareCondition(std::string column, CompareOp op, Value value)
      : column_(std::move(column)), op_(op), value_(std::move(value)) {}

  ConditionType type() const override { return ConditionType::COMPARE; }

  // ---- 子节点 ----
  size_t child_count() const override { return 0; }
  const Condition* child_at(size_t) const override { return nullptr; }

  // ---- 访问器 ----
  const std::string& column() const { return column_; }
  CompareOp op() const { return op_; }
  const Value& value() const { return value_; }

  // ---- Visitor ----
  void accept(ConditionVisitor& visitor) const override {
    visitor.visit(*this);
  }

  // ---- Clone ----
  std::unique_ptr<Condition> clone() const override {
    return std::make_unique<CompareCondition>(*this);
  }

  // ---- 调试 ----
  std::string to_string() const override {
    std::string s = column_;
    s += " " + compare_op_to_string(op_);
    if (!is_null_op(op_)) {
      s += " " + value_.to_string();
    }
    return s;
  }

 private:
  std::string column_;
  CompareOp op_;
  Value value_;
};

// ============================================================
// InCondition：IN / NOT IN
// ============================================================
class InCondition : public Condition {
 public:
  InCondition(std::string column, bool is_not_in, std::vector<Value> values)
      : column_(std::move(column)), is_not_in_(is_not_in),
        values_(std::move(values)) {}

  ConditionType type() const override { return ConditionType::IN; }

  // ---- 子节点 ----
  size_t child_count() const override { return 0; }
  const Condition* child_at(size_t) const override { return nullptr; }

  // ---- 访问器 ----
  const std::string& column() const { return column_; }
  bool is_not_in() const { return is_not_in_; }
  const std::vector<Value>& values() const { return values_; }
  size_t value_count() const { return values_.size(); }

  bool contains(const Value& v) const {
    return std::find(values_.begin(), values_.end(), v) != values_.end();
  }

  // ---- Visitor ----
  void accept(ConditionVisitor& visitor) const override {
    visitor.visit(*this);
  }

  // ---- Clone ----
  std::unique_ptr<Condition> clone() const override {
    return std::make_unique<InCondition>(*this);
  }

  // ---- 调试 ----
  std::string to_string() const override {
    std::string s = column_;
    s += is_not_in_ ? " NOT IN (" : " IN (";
    for (size_t i = 0; i < values_.size(); ++i) {
      if (i > 0) s += ", ";
      s += values_[i].to_string();
    }
    s += ")";
    return s;
  }

 private:
  std::string column_;
  bool is_not_in_;
  std::vector<Value> values_;
};

// ============================================================
// AndCondition：逻辑与（双目）
// ============================================================
class AndCondition : public Condition {
 public:
  AndCondition(ConditionPtr left, ConditionPtr right)
      : left_(std::move(left)), right_(std::move(right)) {}

  ConditionType type() const override { return ConditionType::AND; }

  // ---- 子节点 ----
  size_t child_count() const override { return 2; }
  const Condition* child_at(size_t index) const override {
    return index == 0 ? left_.get() : (index == 1 ? right_.get() : nullptr);
  }

  // ---- 子树访问 ----
  const Condition* left() const { return left_.get(); }
  const Condition* right() const { return right_.get(); }
  Condition* left() { return left_.get(); }
  Condition* right() { return right_.get(); }

  ConditionPtr take_left() { return std::move(left_); }
  ConditionPtr take_right() { return std::move(right_); }

  // ---- Visitor ----
  void accept(ConditionVisitor& visitor) const override {
    visitor.visit(*this);
  }

  // ---- Clone ----
  std::unique_ptr<Condition> clone() const override {
    return std::make_unique<AndCondition>(
        left_ ? left_->clone() : nullptr,
        right_ ? right_->clone() : nullptr);
  }

  // ---- 调试 ----
  std::string to_string() const override {
    return "(" + (left_ ? left_->to_string() : "?") + " AND " +
           (right_ ? right_->to_string() : "?") + ")";
  }

 private:
  ConditionPtr left_;
  ConditionPtr right_;
};

// ============================================================
// OrCondition：逻辑或（双目）
// ============================================================
class OrCondition : public Condition {
 public:
  OrCondition(ConditionPtr left, ConditionPtr right)
      : left_(std::move(left)), right_(std::move(right)) {}

  ConditionType type() const override { return ConditionType::OR; }

  // ---- 子节点 ----
  size_t child_count() const override { return 2; }
  const Condition* child_at(size_t index) const override {
    return index == 0 ? left_.get() : (index == 1 ? right_.get() : nullptr);
  }

  // ---- 子树访问 ----
  const Condition* left() const { return left_.get(); }
  const Condition* right() const { return right_.get(); }
  Condition* left() { return left_.get(); }
  Condition* right() { return right_.get(); }

  ConditionPtr take_left() { return std::move(left_); }
  ConditionPtr take_right() { return std::move(right_); }

  // ---- Visitor ----
  void accept(ConditionVisitor& visitor) const override {
    visitor.visit(*this);
  }

  // ---- Clone ----
  std::unique_ptr<Condition> clone() const override {
    return std::make_unique<OrCondition>(
        left_ ? left_->clone() : nullptr,
        right_ ? right_->clone() : nullptr);
  }

  // ---- 调试 ----
  std::string to_string() const override {
    return "(" + (left_ ? left_->to_string() : "?") + " OR " +
           (right_ ? right_->to_string() : "?") + ")";
  }

 private:
  ConditionPtr left_;
  ConditionPtr right_;
};

// ============================================================
// NotCondition：逻辑非（单目）
// ============================================================
class NotCondition : public Condition {
 public:
  explicit NotCondition(ConditionPtr child)
      : child_(std::move(child)) {}

  ConditionType type() const override { return ConditionType::NOT; }

  // ---- 子节点 ----
  size_t child_count() const override { return 1; }
  const Condition* child_at(size_t index) const override {
    return index == 0 ? child_.get() : nullptr;
  }

  // ---- 子树访问 ----
  const Condition* child() const { return child_.get(); }
  Condition* child() { return child_.get(); }
  ConditionPtr take_child() { return std::move(child_); }

  // ---- Visitor ----
  void accept(ConditionVisitor& visitor) const override {
    visitor.visit(*this);
  }

  // ---- Clone ----
  std::unique_ptr<Condition> clone() const override {
    return std::make_unique<NotCondition>(
        child_ ? child_->clone() : nullptr);
  }

  // ---- 调试 ----
  std::string to_string() const override {
    return "NOT (" + (child_ ? child_->to_string() : "?") + ")";
  }

 private:
  ConditionPtr child_;
};

// ============================================================
// 工厂函数
// ============================================================
inline ConditionPtr make_compare(std::string col, CompareOp op, Value val) {
  return std::make_unique<CompareCondition>(std::move(col), op, std::move(val));
}

inline ConditionPtr make_in(std::string col, bool is_not_in,
                            std::vector<Value> values) {
  return std::make_unique<InCondition>(std::move(col), is_not_in,
                                       std::move(values));
}

inline ConditionPtr make_and(ConditionPtr left, ConditionPtr right) {
  return std::make_unique<AndCondition>(std::move(left), std::move(right));
}

inline ConditionPtr make_or(ConditionPtr left, ConditionPtr right) {
  return std::make_unique<OrCondition>(std::move(left), std::move(right));
}

inline ConditionPtr make_not(ConditionPtr child) {
  return std::make_unique<NotCondition>(std::move(child));
}

// 便捷：从 vector 构建 AND 树
inline ConditionPtr make_and_all(std::vector<ConditionPtr> conditions) {
  if (conditions.empty()) return nullptr;
  ConditionPtr result = std::move(conditions[0]);
  for (size_t i = 1; i < conditions.size(); ++i) {
    result = make_and(std::move(result), std::move(conditions[i]));
  }
  return result;
}

// 便捷：从 vector 构建 OR 树
inline ConditionPtr make_or_all(std::vector<ConditionPtr> conditions) {
  if (conditions.empty()) return nullptr;
  ConditionPtr result = std::move(conditions[0]);
  for (size_t i = 1; i < conditions.size(); ++i) {
    result = make_or(std::move(result), std::move(conditions[i]));
  }
  return result;
}

}  // namespace sql