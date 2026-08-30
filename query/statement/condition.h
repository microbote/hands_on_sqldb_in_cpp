// condition.h
#ifndef QUERY_CONDITION_H
#define QUERY_CONDITION_H

#include <memory>
#include <string>
#include <vector>

#include "relation/sql_relation.h"

namespace query {

// ============================================================
// 比较操作符
// ============================================================
enum class CompareOp { EQ, NE, GT, GE, LT, LE, LIKE, IN };

// ============================================================
// 条件类型
// ============================================================
enum class ConditionType {
  COMPARE,  // 比较: column op value
  AND,      // 逻辑与
  OR,       // 逻辑或
  NOT,      // 逻辑非
  IN        // column IN (value_list)
};

// ============================================================
// 条件表达式树
// ============================================================
class ConditionExpr {
 public:
  // ----- 构造 -----
  // 比较条件: column op value
  explicit ConditionExpr(ConditionType type, const std::string& column, CompareOp op,
                const sql::Value& value);

  // 逻辑条件: AND/OR
  explicit ConditionExpr(ConditionType type, std::unique_ptr<ConditionExpr> left,
                std::unique_ptr<ConditionExpr> right);

  // NOT 条件
  explicit ConditionExpr(
      ConditionType type, std::unique_ptr<ConditionExpr> child);

  // IN 构造
  explicit ConditionExpr(ConditionType type, const std::string& column,
                std::vector<sql::Value> values);

  static ConditionExpr make_compare_expr(const std::string& column, CompareOp op,
                      const sql::Value& value) {
    return ConditionExpr(ConditionType::COMPARE, column, op, value);
  }

  static ConditionExpr make_and_expr(std::unique_ptr<ConditionExpr> left,
                                    std::unique_ptr<ConditionExpr> right) {
    return ConditionExpr(ConditionType::AND, std::move(left), std::move(right));
  }

  static ConditionExpr make_or_expr(std::unique_ptr<ConditionExpr> left,
                                   std::unique_ptr<ConditionExpr> right) {
    return ConditionExpr(ConditionType::OR, std::move(left), std::move(right));
  }

  static ConditionExpr make_not_expr(std::unique_ptr<ConditionExpr> child) {
    return ConditionExpr(ConditionType::NOT, std::move(child));
  }

  static ConditionExpr make_in_expr(const std::string& column,
                                   std::vector<sql::Value> values) {
    return ConditionExpr(ConditionType::IN, column, std::move(values));
  }

  // 拷贝/移动
  ConditionExpr(const ConditionExpr& other);
  ConditionExpr(ConditionExpr&& other) noexcept;
  ConditionExpr& operator=(const ConditionExpr& other);
  ConditionExpr& operator=(ConditionExpr&& other) noexcept;

  // ----- 访问 -----
  ConditionType type() const { return type_; }
  bool is_compare() const { return type_ == ConditionType::COMPARE; }
  bool is_and() const { return type_ == ConditionType::AND; }
  bool is_or() const { return type_ == ConditionType::OR; }
  bool is_not() const { return type_ == ConditionType::NOT; }
  bool is_in() const { return type_ == ConditionType::IN; }

  const std::string& column() const { return column_; }
  CompareOp op() const { return op_; }
  const sql::Value& value() const { return value_; }
  const ConditionExpr* left() const { return left_.get(); }
  const ConditionExpr* right() const { return right_.get(); }
  const std::vector<sql::Value>& in_values() const { return in_values_; }

  // ----- 核心方法 -----
  // 判断行是否匹配条件
  bool matches(const sql::Row& row, const sql::TableSchema& schema) const;

  // 提取主键条件（用于优化）
  bool extract_primary_key_conditions(
      const sql::TableSchema& schema,
      std::vector<std::pair<CompareOp, sql::Value>>& pk_conds,
      std::unique_ptr<ConditionExpr>& remaining) const;

  // 检查是否只涉及主键
  bool only_primary_key(const sql::TableSchema& schema) const;

  // ----- 调试 -----
  std::string to_string() const;

 private:
  bool matches_compare(const sql::Row& row,
                       const sql::TableSchema& schema) const;
  bool compare_values(const sql::Value& row_val) const;
  bool compare_in_values(const sql::Value& row_val) const;
  bool like_match(const std::string& str, const std::string& pattern) const;
  void collect_pk_conditions(
      const sql::TableSchema& schema,
      std::vector<std::pair<CompareOp, sql::Value>>& result) const;

  ConditionType type_;

  // COMPARE 类型使用
  std::string column_;
  CompareOp op_;
  sql::Value value_;
  std::vector<sql::Value> in_values_;  // IN 列表

  // 逻辑类型使用
  std::unique_ptr<ConditionExpr> left_;
  std::unique_ptr<ConditionExpr> right_;
};

// ============================================================
// 辅助函数
// ============================================================
std::string compare_op_to_string(CompareOp op);
CompareOp string_to_compare_op(const std::string& str);

}  // namespace query

#endif  // QUERY_CONDITION_H