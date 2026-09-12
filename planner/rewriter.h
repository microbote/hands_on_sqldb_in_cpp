// rewriter.h
#ifndef REWRITER_H
#define REWRITER_H

#include <memory>
#include <vector>

#include "statement.h"

namespace sql {

// 查询重写：简化优化查询
class QueryRewriter {
 public:
  QueryRewriter() = default;
  ~QueryRewriter() = default;

  // 重写 Statement
  std::unique_ptr<Statement> rewrite(Statement* stmt);

 private:
  // 条件重写
  std::vector<Condition> rewrite_conditions(
      const std::vector<Condition>& conditions);

  // 常量折叠：age > 10 AND age > 20 → age > 20
  std::vector<Condition> fold_constants(const std::vector<Condition>& conds);

  // 去重条件
  std::vector<Condition> deduplicate(const std::vector<Condition>& conds);

  // 合并条件
  std::vector<Condition> merge_ranges(const std::vector<Condition>& conds);

  // 判断条件是否可以合并
  bool can_merge(const Condition& a, const Condition& b);
  Condition merge_two(const Condition& a, const Condition& b);

  // 检查条件是否为同一列
  bool same_column(const Condition& a, const Condition& b);
};

}  // namespace sql

#endif  // REWRITER_H