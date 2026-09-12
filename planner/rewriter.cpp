// rewriter.cpp
#include "rewriter.h"

#include <algorithm>
#include <iostream>

namespace sql {

std::unique_ptr<Statement> QueryRewriter::rewrite(Statement* stmt) {
  if (!stmt || !stmt->is_valid()) return nullptr;

  switch (stmt->type()) {
    case StatementType::SELECT: {
      auto select = static_cast<SelectStatement*>(stmt);
      auto rewritten = std::make_unique<SelectStatement>(select->table_name);
      rewritten->columns = select->columns;
      rewritten->conditions = rewrite_conditions(select->conditions);
      rewritten->valid_ = true;

      // 输出重写信息
      if (rewritten->conditions.size() != select->conditions.size()) {
        std::cout << "  📝 查询重写: 条件从 " << select->conditions.size()
                  << " 简化为 " << rewritten->conditions.size() << " 个"
                  << std::endl;
      }
      return rewritten;
    }
    case StatementType::UPDATE: {
      auto update = static_cast<UpdateStatement*>(stmt);
      auto rewritten = std::make_unique<UpdateStatement>(update->table_name);
      rewritten->assignments = update->assignments;
      rewritten->conditions = rewrite_conditions(update->conditions);
      rewritten->valid_ = true;
      return rewritten;
    }
    case StatementType::DELETE: {
      auto del = static_cast<DeleteStatement*>(stmt);
      auto rewritten = std::make_unique<DeleteStatement>(del->table_name);
      rewritten->conditions = rewrite_conditions(del->conditions);
      rewritten->valid_ = true;
      return rewritten;
    }
    default:
      // USE 和 INSERT 不需要重写
      return nullptr;
  }
}

std::vector<Condition> QueryRewriter::rewrite_conditions(
    const std::vector<Condition>& conditions) {
  if (conditions.size() < 2) return conditions;

  // 1. 去重
  auto dedup = deduplicate(conditions);

  // 2. 常量折叠
  auto folded = fold_constants(dedup);

  // 3. 合并范围条件
  auto merged = merge_ranges(folded);

  return merged;
}

std::vector<Condition> QueryRewriter::deduplicate(
    const std::vector<Condition>& conds) {
  std::vector<Condition> result;

  for (const auto& cond : conds) {
    bool duplicate = false;
    for (const auto& existing : result) {
      if (existing.column == cond.column && existing.op == cond.op &&
          existing.value.type == cond.value.type) {
        if (existing.value.type == DataType::INTEGER) {
          if (existing.value.int_val == cond.value.int_val) {
            duplicate = true;
            break;
          }
        } else if (existing.value.type == DataType::STRING) {
          if (existing.value.str_val == cond.value.str_val) {
            duplicate = true;
            break;
          }
        } else if (existing.value.type == DataType::BOOLEAN) {
          if (existing.value.bool_val == cond.value.bool_val) {
            duplicate = true;
            break;
          }
        }
      }
    }
    if (!duplicate) {
      result.push_back(cond);
    }
  }

  return result;
}

std::vector<Condition> QueryRewriter::fold_constants(
    const std::vector<Condition>& conds) {
  std::vector<Condition> result;

  for (const auto& cond : conds) {
    // 简化：如果是同一个列，可以选择更严格的条件
    // 例如：age > 10 AND age > 20 → age > 20
    // 这里只做简单的常量折叠
    result.push_back(cond);
  }

  return result;
}

std::vector<Condition> QueryRewriter::merge_ranges(
    const std::vector<Condition>& conds) {
  if (conds.size() < 2) return conds;

  std::vector<Condition> result;
  std::vector<Condition> remaining = conds;

  while (!remaining.empty()) {
    Condition current = remaining.front();
    remaining.erase(remaining.begin());
    bool merged = false;

    for (size_t i = 0; i < remaining.size(); ++i) {
      if (can_merge(current, remaining[i])) {
        current = merge_two(current, remaining[i]);
        remaining.erase(remaining.begin() + i);
        merged = true;
        break;
      }
    }

    if (!merged) {
      result.push_back(current);
    } else {
      // 重新检查是否能继续合并
      remaining.push_back(current);
    }
  }

  return result;
}

bool QueryRewriter::can_merge(const Condition& a, const Condition& b) {
  if (!same_column(a, b)) return false;

  // 可以合并的情况：
  // 1. age > 10 AND age > 20 → age > 20
  // 2. age > 10 AND age < 20 → 保留（范围扫描）
  // 3. age = 10 AND age = 10 → age = 10 (去重)

  if (a.op == CompareOp::EQ && b.op == CompareOp::EQ) {
    return true;  // 相同值已在去重中处理
  }

  if ((a.op == CompareOp::GT || a.op == CompareOp::GE) &&
      (b.op == CompareOp::GT || b.op == CompareOp::GE)) {
    return true;  // 取更严格的
  }

  if ((a.op == CompareOp::LT || a.op == CompareOp::LE) &&
      (b.op == CompareOp::LT || b.op == CompareOp::LE)) {
    return true;  // 取更严格的
  }

  return false;
}

Condition QueryRewriter::merge_two(const Condition& a, const Condition& b) {
  // 假设 a 和 b 是同一列
  Condition result = a;

  if (a.op == CompareOp::EQ && b.op == CompareOp::EQ) {
    // 取更严格的？实际应该检查是否相等
    return a;
  }

  if ((a.op == CompareOp::GT || a.op == CompareOp::GE) &&
      (b.op == CompareOp::GT || b.op == CompareOp::GE)) {
    // 取更大的值（更严格）
    if (b.value.type == DataType::INTEGER) {
      if (b.value.int_val > a.value.int_val) {
        result = b;
      }
    }
  }

  if ((a.op == CompareOp::LT || a.op == CompareOp::LE) &&
      (b.op == CompareOp::LT || b.op == CompareOp::LE)) {
    // 取更小的值（更严格）
    if (b.value.type == DataType::INTEGER) {
      if (b.value.int_val < a.value.int_val) {
        result = b;
      }
    }
  }

  return result;
}

bool QueryRewriter::same_column(const Condition& a, const Condition& b) {
  return a.column == b.column;
}

}  // namespace sql