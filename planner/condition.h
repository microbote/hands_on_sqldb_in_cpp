#ifndef QUERY_CONDITION_H
#define QUERY_CONDITION_H

#include <algorithm>
#include <cassert>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "compare_op.h"

#include "relation/sql_relation.h"

struct ASTNode;

namespace stmt {

class ConditionExpr;
struct ConditionExtractResult {
  std::unique_ptr<ConditionExpr> pk_cond;    // 主键条件树
  std::unique_ptr<ConditionExpr> remaining;  // 剩余条件树

  // pk_cond AND remaining = 原始条件
  bool has_pk() const { return pk_cond != nullptr; }
  bool has_remaining() const { return remaining != nullptr; }
};

// ============================================================
// ConditionType
// ============================================================
enum class ConditionType {
  COMPARE,  // leaf
  IN,       // leaf
  AND,      // non-leaf
  OR,       // non-leaf
  NOT,      // non-leaf
  UNKNOWN = 99
};


class ConditionExpr {
 public:
  // ---- 构造 ----
  ConditionExpr(const std::string& column, CompareOp op, sql::Value value);
  ConditionExpr(ConditionType type, std::unique_ptr<ConditionExpr> left,
                std::unique_ptr<ConditionExpr> right);
  ConditionExpr(ConditionType type, std::unique_ptr<ConditionExpr> child);
  ConditionExpr(ConditionType type, const std::string& column,
                std::vector<sql::Value> values);

  // ---- 工厂方法 ----
  static ConditionExpr make_compare(const std::string& column, CompareOp op,
                                    sql::Value value);
  static ConditionExpr make_in(const std::string& column,
                               std::vector<sql::Value> values);

  static ConditionExpr make_and(std::unique_ptr<ConditionExpr> left,
                                std::unique_ptr<ConditionExpr> right);
  static ConditionExpr make_or(std::unique_ptr<ConditionExpr> left,
                               std::unique_ptr<ConditionExpr> right);
  static ConditionExpr make_not(std::unique_ptr<ConditionExpr> child);

  // ---- 拷贝 / 移动 ----
  ConditionExpr(const ConditionExpr& other);
  ConditionExpr(ConditionExpr&& other) noexcept;
  ConditionExpr& operator=(const ConditionExpr& other);
  ConditionExpr& operator=(ConditionExpr&& other) noexcept;
  std::unique_ptr<ConditionExpr> clone() const;

  // ---- 访问器 ----
  ConditionType type() const { return type_; }
  const std::string& column() const { return column_; }
  CompareOp op() const { return op_; }
  const sql::Value& value() const { return value_; }
  const std::vector<sql::Value>& in_values() const { return in_values_; }
  const ConditionExpr* left() const { return left_.get(); }
  const ConditionExpr* right() const { return right_.get(); }
  std::unique_ptr<ConditionExpr> move_left() { return std::move(left_); }
  std::unique_ptr<ConditionExpr> move_right() { return std::move(right_); }

  bool is_compare() const { return type_ == ConditionType::COMPARE; }
  bool is_and() const { return type_ == ConditionType::AND; }
  bool is_or() const { return type_ == ConditionType::OR; }
  bool is_not() const { return type_ == ConditionType::NOT; }
  bool is_in() const { return type_ == ConditionType::IN; }
  bool is_unknown() const { return type_ == ConditionType::UNKNOWN; }
  bool is_leaf() const { return is_compare() || is_in(); }
  bool is_internal() const { return is_and() || is_or() || is_not(); }

  // ---- 求值 ----
  bool involve_only_primary_key(const sql::TableSchema& schema) const;

  bool match_row(const sql::Row& row, const sql::TableSchema& schema) const;

  // ---- 核心：主键条件提取 ----
  // ============================================================
  // ConditionExpr
  // Lazy: build完成后并不做优化，只有在提取主键条件时才做优化
  // 处理流水线（extract_primary_key_conditions 内部）：
  //   Step 1: NOT 下推（De Morgan）
  //   Step 2: 规范化为左闭右开（LE→LT, GT→GE）
  //   Step 3: 递归提取主键条件
  // ============================================================
  // ---- 优化（返回新表达式） ----
  // 返回优化后的条件表达式副本
  std::unique_ptr<ConditionExpr> optimize(const sql::TableSchema& schema) const {
    auto cloned = clone();
    return optimize(std::move(cloned), schema);
  }

  std::unique_ptr<ConditionExpr> pushdown_not() const {
    auto cloned = clone();
    return pushdown_not(std::move(cloned), false);
  }

  std::unique_ptr<ConditionExpr> normalize_boundary(const sql::TableSchema& schema) const {
    auto cloned = clone();
    return normalize_boundary(std::move(cloned), schema.primary_key_name());
  }

  std::unique_ptr<ConditionExpr> simplify() const {
    auto cloned = clone();
    return simplify(std::move(cloned));
  }

  // ---- 主键条件提取 ----
  // 内部会调用 optimize() 进行优化，然后提取
  ConditionExtractResult extract_primary_key_conditions(
      const sql::TableSchema& schema) const;

  // ---- 调试 ----
  std::string to_string() const;

 private:
  // ---- 匹配辅助 ----
  bool is_only_primary_key(const std::string& pk) const;

  sql::Value get_column_value(const sql::Row& row,
                              const sql::TableSchema& schema) const;
  bool match_in_expr(const sql::Value& row_val) const;

  // ---- 内部优化方法，都是移动语义，使用者需要先clone再传入 ----
  static std::unique_ptr<ConditionExpr> pushdown_not(
      std::unique_ptr<ConditionExpr> expr,
      bool need_flip);  // De Morgan 下推
  static std::unique_ptr<ConditionExpr> normalize_boundary(
      std::unique_ptr<ConditionExpr> expr, const std::string& pk);  // 规范化为左闭右开
  static std::unique_ptr<ConditionExpr> simplify(
      std::unique_ptr<ConditionExpr> expr);  // 简化表达式
  static std::unique_ptr<ConditionExpr> optimize(
      std::unique_ptr<ConditionExpr> expr, const sql::TableSchema& schema);

  // 辅助函数
  static bool is_true_expr(const ConditionExpr& expr);
  static bool is_false_expr(const ConditionExpr& expr);
  static std::unique_ptr<ConditionExpr> make_true_expr();
  static std::unique_ptr<ConditionExpr> make_false_expr();
  static bool contains_expr(const ConditionExpr& container,
                            const ConditionExpr& target);

  static void flatten_or(const ConditionExpr* expr,
                         std::vector<const ConditionExpr*>& leaves);
  static void group_eq_by_column(
      const std::vector<const ConditionExpr*>& leaves,
      std::unordered_map<std::string, std::vector<const ConditionExpr*>>&
          groups);
  static std::unique_ptr<ConditionExpr> build_optimized_or(
      const std::vector<const ConditionExpr*>& leaves,
      const std::unordered_map<std::string, std::vector<const ConditionExpr*>>&
          eq_groups);

  static std::unique_ptr<ConditionExpr> merge_and_compare_conditions(
      const ConditionExpr& a, const ConditionExpr& b);
  static std::unique_ptr<ConditionExpr> merge_or_compare_conditions(
      const ConditionExpr& a, const ConditionExpr& b);

  static std::unique_ptr<ConditionExpr> simplify_or(
      std::unique_ptr<ConditionExpr> expr_or);
  static std::unique_ptr<ConditionExpr> simplify_and(
      std::unique_ptr<ConditionExpr> expr_and);
  static std::unique_ptr<ConditionExpr> simplify_not(
      std::unique_ptr<ConditionExpr> expr_not);

  // ---- 内部提取方法 ----
  static ConditionExtractResult do_extract_pk(
      std::unique_ptr<ConditionExpr> expr, const sql::TableSchema& schema);

  ConditionType type_;
  std::string column_;
  CompareOp op_ = CompareOp::UNKNOWN;
  sql::Value value_;
  std::vector<sql::Value> in_values_;
  std::unique_ptr<ConditionExpr> left_;
  std::unique_ptr<ConditionExpr> right_;
};

}  // namespace query

#endif  // QUERY_CONDITION_H