// condition.h
#ifndef QUERY_CONDITION_H
#define QUERY_CONDITION_H

#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "relation/sql_relation.h"

namespace query {

// ============================================================
// 比较操作符（仅用于 COMPARE 节点）
// ============================================================
enum class CompareOp { EQ, NE, GT, GE, LT, LE, LIKE, IS_NULL, IS_NOT_NULL };

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
// 主键条件片段（一个 OR 分支）
// ============================================================
struct PrimaryKeyFragment {
  bool is_point_set = false;
  std::vector<sql::Value> points;   // 点集（OR 关系）
  std::optional<sql::Value> start;  // 范围起始（包含）
  std::optional<sql::Value> end;    // 范围结束（不包含）

  bool is_valid() const;
  bool is_point_set_only() const { return is_point_set && !points.empty(); }
  bool is_range() const { return !is_point_set && (start || end); }
  bool is_all() const { return !is_point_set && !start && !end; }

  bool operator==(const PrimaryKeyFragment& other) const;

  std::string to_string() const;

  // and 总是可以合并，大不了交集为空
  static std::optional<PrimaryKeyFragment> merge_fragments_and(
      const PrimaryKeyFragment& a, const PrimaryKeyFragment& b);

  // or 可能无法合并，比如一个range,一个点集
  // 若无法合并，则返回 nullopt, 需要push_back二者
  static std::optional<PrimaryKeyFragment> can_merge_fragments_or(
      const PrimaryKeyFragment& a, const PrimaryKeyFragment& b);

  static std::optional<PrimaryKeyFragment> can_merge_ranges_or(
      const PrimaryKeyFragment& a, const PrimaryKeyFragment& b);

  static std::optional<PrimaryKeyFragment> merge_fragments_or_point_set(
    const PrimaryKeyFragment& a, const PrimaryKeyFragment& b);

  static std::vector<PrimaryKeyFragment> merge_vector_ranges_or(
      std::vector<PrimaryKeyFragment>& fragments
  );

  static std::optional<PrimaryKeyFragment> merge_vector_point_set_or(
      const std::vector<PrimaryKeyFragment>& fragments);

};

// ============================================================
// 主键条件（多个 OR 片段）
// ============================================================
struct PrimaryKeyCondition {
  std::vector<PrimaryKeyFragment> fragments;  // OR 关系

  bool empty() const { return fragments.empty(); }
  bool is_single() const { return fragments.size() == 1; }
  bool has_pk_condition() const { return !empty(); }
  size_t size() const { return fragments.size(); }

  // 合并两个条件（AND 语义），返回 nullopt 表示无法合并
  static std::optional<PrimaryKeyCondition> merge_and(
      const PrimaryKeyCondition& a, const PrimaryKeyCondition& b);

  // 合并两个条件（OR 语义）
  static PrimaryKeyCondition merge_or(const PrimaryKeyCondition& a,
                                      const PrimaryKeyCondition& b);

  // 简化（合并重叠的点集/范围）
  static PrimaryKeyCondition simplify(const PrimaryKeyCondition& cond);



  std::string to_string() const;
};

// ============================================================
// 扫描规范（主键条件 + ORDER BY + LIMIT）
// ============================================================
struct ScanSpec {
  PrimaryKeyCondition pk_cond;
  bool order_by_pk = false;
  bool ascending = true;
  size_t limit = 0;

  bool has_pk_condition() const { return pk_cond.has_pk_condition(); }
  bool is_single_range() const { return pk_cond.is_single(); }

  std::string to_string() const;
};

// ============================================================
// 条件抽取结果
// ============================================================
class ConditionExpr;
struct ConditionExtractResult {
  PrimaryKeyCondition pk_cond;
  std::unique_ptr<ConditionExpr> remaining;  // 非主键条件

  ConditionExtractResult() = default;
  bool has_pk_condition() const { return pk_cond.has_pk_condition(); }
};

// ============================================================
// 条件表达式树
// ============================================================
class ConditionExpr {
 public:
  // ----- 构造 -----
  // COMPARE: column op value
  ConditionExpr(const std::string& column, CompareOp op,
                const sql::Value& value);

  // AND/OR: left op right
  ConditionExpr(ConditionType type, std::unique_ptr<ConditionExpr> left,
                std::unique_ptr<ConditionExpr> right);

  // NOT: child
  ConditionExpr(ConditionType type, std::unique_ptr<ConditionExpr> child);

  // IN: column IN (values)
  ConditionExpr(ConditionType type, const std::string& column,
                std::vector<sql::Value> values);

  // ----- 静态工厂 -----
  static ConditionExpr make_compare(const std::string& column, CompareOp op,
                                    const sql::Value& value);
  static ConditionExpr make_and(std::unique_ptr<ConditionExpr> left,
                                std::unique_ptr<ConditionExpr> right);
  static ConditionExpr make_or(std::unique_ptr<ConditionExpr> left,
                               std::unique_ptr<ConditionExpr> right);
  static ConditionExpr make_not(std::unique_ptr<ConditionExpr> child);
  static ConditionExpr make_in(const std::string& column,
                               std::vector<sql::Value> values);

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
  bool match(const sql::Row& row, const sql::TableSchema& schema) const;

  // 抽取主键条件（返回结构化结果）
  ConditionExtractResult extract_primary_key_conditions(
      const sql::TableSchema& schema);

  // 检查是否只涉及主键
  bool only_primary_key(const sql::TableSchema& schema) const;

  std::string to_string() const;

 private:
  bool matches_compare(const sql::Row& row,
                       const sql::TableSchema& schema) const;
  bool compare_values(const sql::Value& row_val) const;
  bool like_match(const std::string& str, const std::string& pattern) const;
  bool compare_in_values(const sql::Value& row_val) const;

  ConditionType type_;
  std::string column_;
  CompareOp op_;
  sql::Value value_;
  std::vector<sql::Value> in_values_;
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