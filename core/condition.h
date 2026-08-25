// condition.h
#ifndef CONDITION_H
#define CONDITION_H

#include <memory>
#include <string>
#include <vector>

#include "storage_engine.h"

namespace sql {

// ============ 比较操作符 ============
enum class CompareOp {
  EQ,   // =
  NE,   // !=
  GT,   // >
  GE,   // >=
  LT,   // <
  LE,   // <=
  LIKE  // LIKE
};

// ============ 条件类型 ============
enum class ConditionType {
  COMPARE,  // 比较: column op value
  AND,      // 逻辑与
  OR,       // 逻辑或
  NOT       // 逻辑非
};

// ============ 条件表达式节点 ============
class ConditionExpr {
 public:
  // ---- 构造 ----
  // 比较条件
  ConditionExpr(const std::string& col, CompareOp op,
                const storage::Value& val);

  // 逻辑条件 (AND/OR)
  ConditionExpr(ConditionType type, std::unique_ptr<ConditionExpr> left,
                std::unique_ptr<ConditionExpr> right);

  // NOT 条件
  explicit ConditionExpr(std::unique_ptr<ConditionExpr> child);

  // 拷贝/移动
  ConditionExpr(const ConditionExpr& other);
  ConditionExpr(ConditionExpr&& other) noexcept;
  ConditionExpr& operator=(const ConditionExpr& other);
  ConditionExpr& operator=(ConditionExpr&& other) noexcept;

  // ---- 访问 ----
  ConditionType type() const { return type_; }
  bool is_compare() const { return type_ == ConditionType::COMPARE; }
  bool is_and() const { return type_ == ConditionType::AND; }
  bool is_or() const { return type_ == ConditionType::OR; }
  bool is_not() const { return type_ == ConditionType::NOT; }

  // 比较条件访问
  const std::string& column() const { return column_; }
  CompareOp op() const { return op_; }
  const storage::Value& value() const { return value_; }

  // 子节点访问
  const ConditionExpr* left() const { return left_.get(); }
  const ConditionExpr* right() const { return right_.get(); }

  // ---- 核心方法 ----
  // 判断行是否匹配条件
  bool matches(const storage::Row& row,
               const storage::TableSchema& schema) const;

  // 转换为字符串（调试用）
  std::string to_string() const;

  // 检查是否只涉及主键（用于索引优化）
  bool only_primary_key(const std::string& pk_column) const;

  // 提取主键条件
  bool extract_primary_key_conditions(
      const std::string& pk_column,
      std::vector<std::pair<CompareOp, storage::Value>>& conditions) const;

 private:
  // ---- 内部方法 ----
  bool matches_compare(const storage::Row& row,
                       const storage::TableSchema& schema) const;
  bool compare_values(const storage::Value& row_val) const;
  bool like_match(const std::string& str, const std::string& pattern) const;

  void collect_primary_key_conditions(
      const std::string& pk_column,
      std::vector<std::pair<CompareOp, storage::Value>>& result) const;

  // ---- 成员 ----
  ConditionType type_;

  // COMPARE 类型使用
  std::string column_;
  CompareOp op_;
  storage::Value value_;

  // 逻辑类型使用
  std::unique_ptr<ConditionExpr> left_;
  std::unique_ptr<ConditionExpr> right_;
};

// ============ 辅助：操作符转字符串 ============
std::string compare_op_to_string(CompareOp op);
CompareOp string_to_compare_op(const std::string& str);

}  // namespace sql

#endif  // CONDITION_H