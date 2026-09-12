// rewriter.cpp
//
// 条件树重写：把 WHERE 变成"更容易被优化器吃下去"的等价形式。
//
// 流水线（rewrite_condition_tree）：
//   pushdown_not -> flattern -> fold_constants -> deduplicate -> simplify
//
// 与旧实现（planner/old/condition.cpp）的关键差异：
//   1) 不再做"字符串比较 + value±1"的区间折叠。区间代数（NULL / ±∞ / 开闭 /
//      溢出）统一由 KeyRange 负责，它是 key 空间的唯一权威；重写器只做
//      **结构** 化简：去重、吸收律、NOT 下推、扁平化、OR-of-EQ -> IN。
//      旧实现的 `<=` 转 `< value+1` 在 INT64_MAX 上会溢出，且把字符串比较
//      当数值比较，正是要避免的。
//   2) 不引入 TRUE/FALSE 常量节点（旧实现用 "1=1"/"1=0" 当哨兵）。
//      矛盾条件（id>5 AND id<3）原样保留，由 Optimizer 转成空 KeyRange；
//      表达"真"用 nullptr（无条件），不需要第三种表示。
//   3) NOT (x IN (..)) 直接变成 x NOT IN (..)：InCondition 有 is_not_in 标志，
//      两者三值语义完全一致（NULL 参与时都是 UNKNOWN）。
//
// 所有 pass 都是纯函数：入参只读，返回新树，不改调用方的 Query。
#include "rewriter.h"

#include <algorithm>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

#include "sql_types/compare_op.h"
#include "sql_types/condition_types.h"

namespace plan {
namespace {

using sql::CompareOp;
using sql::Condition;
using sql::ConditionPtr;
using sql::ConditionType;

ConditionPtr clone_or_null(const Condition *node) {
  return node != nullptr ? node->clone() : nullptr;
}

// 结构相等：类型 + 文本形式。用于去重与吸收律
// （条件树很小，O(n²) 的字符串比较完全够用，不值得为它引入哈希设计）
bool same_node(const Condition *a, const Condition *b) {
  return a->type() == b->type() && a->to_string() == b->to_string();
}

// 收集同一逻辑连接词的子节点（扁平化）；遇到别的类型就当作一个子项
void collect_same_op(const Condition *node, ConditionType type,
                     std::vector<const Condition *> &out) {
  if (node == nullptr) {
    return;
  }
  if (node->type() == type) {
    for (size_t i = 0; i < node->child_count(); ++i) {
      collect_same_op(node->child_at(i), type, out);
    }
    return;
  }
  out.push_back(node);
}

// 左深重建：((a AND b) AND c) AND d
ConditionPtr rebuild(ConditionType type, std::vector<ConditionPtr> children) {
  ConditionPtr result;
  for (auto &child : children) {
    if (!child) {
      continue;
    }
    if (!result) {
      result = std::move(child);
      continue;
    }
    result = (type == ConditionType::AND)
                 ? sql::make_and(std::move(result), std::move(child))
                 : sql::make_or(std::move(result), std::move(child));
  }
  return result;
}

// container 子树里是否有与 target 结构相等的节点
bool tree_contains(const Condition &container, const Condition &target) {
  if (same_node(&container, &target)) {
    return true;
  }
  for (size_t i = 0; i < container.child_count(); ++i) {
    if (tree_contains(*container.child_at(i), target)) {
      return true;
    }
  }
  return false;
}

// ============================================================
// 1) NOT 下推（De Morgan）
//    NOT(a AND b) -> NOT a OR NOT b
//    NOT(a > 5)   -> a <= 5      （叶子交给 flip_compare_op）
// ============================================================
ConditionPtr pushdown_not_impl(const Condition *node, bool negated) {
  if (node == nullptr) {
    return nullptr;
  }
  switch (node->type()) {
  case ConditionType::NOT: {
    const auto &not_node = static_cast<const sql::NotCondition &>(*node);
    return pushdown_not_impl(not_node.child(), !negated);
  }
  case ConditionType::AND: {
    const auto &and_node = static_cast<const sql::AndCondition &>(*node);
    auto left = pushdown_not_impl(and_node.left(), negated);
    auto right = pushdown_not_impl(and_node.right(), negated);
    return negated ? sql::make_or(std::move(left), std::move(right))
                   : sql::make_and(std::move(left), std::move(right));
  }
  case ConditionType::OR: {
    const auto &or_node = static_cast<const sql::OrCondition &>(*node);
    auto left = pushdown_not_impl(or_node.left(), negated);
    auto right = pushdown_not_impl(or_node.right(), negated);
    return negated ? sql::make_and(std::move(left), std::move(right))
                   : sql::make_or(std::move(left), std::move(right));
  }
  case ConditionType::COMPARE: {
    const auto &cmp = static_cast<const sql::CompareCondition &>(*node);
    if (!negated) {
      return node->clone();
    }
    const CompareOp flipped = sql::flip_compare_op(cmp.op());
    if (flipped == CompareOp::UNKNOWN) {
      return sql::make_not(node->clone()); // 无法翻转的算子：保留 NOT
    }
    return sql::make_compare(cmp.column(), flipped, cmp.value());
  }
  case ConditionType::IN: {
    const auto &in = static_cast<const sql::InCondition &>(*node);
    if (!negated) {
      return node->clone();
    }
    // NOT (x IN (..)) 与 x NOT IN (..) 的 SQL 三值语义完全一致
    return sql::make_in(in.column(), !in.is_not_in(), in.values());
  }
  }
  return node->clone();
}

// ============================================================
// 2) 扁平化：把 (a AND (b AND c)) 变成 ((a AND b) AND c)
//    目的不是"更好看"，而是让去重/吸收/合并看到同一层的所有子项。
// ============================================================
ConditionPtr flatten_impl(const Condition *node) {
  if (node == nullptr) {
    return nullptr;
  }
  switch (node->type()) {
  case ConditionType::AND:
  case ConditionType::OR: {
    const ConditionType type = node->type();
    std::vector<const Condition *> flat;
    collect_same_op(node, type, flat);
    std::vector<ConditionPtr> children;
    children.reserve(flat.size());
    for (const Condition *child : flat) {
      children.push_back(flatten_impl(child));
    }
    return rebuild(type, std::move(children));
  }
  case ConditionType::NOT: {
    const auto &not_node = static_cast<const sql::NotCondition &>(*node);
    return sql::make_not(flatten_impl(not_node.child()));
  }
  default:
    return node->clone();
  }
}

// ============================================================
// 3) 常量折叠（本层能做的部分）
//    - IN 列表去重：x IN (1, 1, 2) -> x IN (1, 2)
//    - 单元素 IN 退化成比较：x IN (5) -> x = 5，x NOT IN (5) -> x != 5
//    （NULL 的单元素不做退化：保留 IN 形式更直观，语义不变）
// ============================================================
ConditionPtr fold_constants_impl(const Condition *node) {
  if (node == nullptr) {
    return nullptr;
  }
  switch (node->type()) {
  case ConditionType::AND: {
    const auto &n = static_cast<const sql::AndCondition &>(*node);
    return sql::make_and(fold_constants_impl(n.left()),
                         fold_constants_impl(n.right()));
  }
  case ConditionType::OR: {
    const auto &n = static_cast<const sql::OrCondition &>(*node);
    return sql::make_or(fold_constants_impl(n.left()),
                        fold_constants_impl(n.right()));
  }
  case ConditionType::NOT: {
    const auto &n = static_cast<const sql::NotCondition &>(*node);
    return sql::make_not(fold_constants_impl(n.child()));
  }
  case ConditionType::IN: {
    const auto &n = static_cast<const sql::InCondition &>(*node);
    std::vector<sql::Value> values;
    for (const auto &v : n.values()) {
      if (std::find(values.begin(), values.end(), v) == values.end()) {
        values.push_back(v);
      }
    }
    if (values.size() == 1 && !values.front().is_null()) {
      return sql::make_compare(n.column(),
                               n.is_not_in() ? CompareOp::NE : CompareOp::EQ,
                               values.front());
    }
    return sql::make_in(n.column(), n.is_not_in(), std::move(values));
  }
  default:
    return node->clone();
  }
}

// ============================================================
// 4) 去重：同层的子项结构相等时只留一个
//    x = 1 AND x = 1            -> x = 1
//    x = 1 OR  x = 1            -> x = 1
//    (a AND b) OR (a AND b)     -> a AND b
// ============================================================
ConditionPtr deduplicate_impl(const Condition *node) {
  if (node == nullptr) {
    return nullptr;
  }
  switch (node->type()) {
  case ConditionType::AND:
  case ConditionType::OR: {
    const ConditionType type = node->type();
    std::vector<const Condition *> flat;
    collect_same_op(node, type, flat);

    std::vector<ConditionPtr> children;
    for (const Condition *child : flat) {
      ConditionPtr rewritten = deduplicate_impl(child);
      bool duplicate = false;
      for (const auto &kept : children) {
        if (same_node(kept.get(), rewritten.get())) {
          duplicate = true;
          break;
        }
      }
      if (!duplicate) {
        children.push_back(std::move(rewritten));
      }
    }
    return rebuild(type, std::move(children));
  }
  case ConditionType::NOT: {
    const auto &n = static_cast<const sql::NotCondition &>(*node);
    return sql::make_not(deduplicate_impl(n.child()));
  }
  default:
    return node->clone();
  }
}

// ============================================================
// 5) 化简：吸收律 + OR-of-EQ -> IN + NOT NOT
//    A OR (A AND B)      -> A
//    A AND (A OR B)      -> A
//    x = 1 OR x = 2      -> x IN (1, 2)
// ============================================================
ConditionPtr simplify_impl(const Condition *node);

// AND：丢掉"被其它合取项完全包含的 OR 项"（吸收律）
ConditionPtr simplify_and(const Condition *node) {
  std::vector<const Condition *> flat;
  collect_same_op(node, ConditionType::AND, flat);

  std::vector<ConditionPtr> children;
  for (const Condition *child : flat) {
    children.push_back(simplify_impl(child));
  }

  std::vector<ConditionPtr> kept;
  // 先判定、后搬移：判定阶段要看全部兄弟节点，提前 move 会把它们置空
  std::vector<bool> absorbed(children.size(), false);
  for (size_t i = 0; i < children.size(); ++i) {
    if (!children[i]->is_or()) {
      continue;
    }
    for (size_t j = 0; j < children.size(); ++j) {
      if (i == j) {
        continue;
      }
      // A AND (A OR B) -> A：OR 分支里出现了另一个合取项
      if (tree_contains(*children[i], *children[j])) {
        absorbed[i] = true;
        break;
      }
    }
  }
  for (size_t i = 0; i < children.size(); ++i) {
    if (!absorbed[i]) {
      kept.push_back(std::move(children[i]));
    }
  }
  return rebuild(ConditionType::AND, std::move(kept));
}

// OR：吸收律 + 同列 EQ 合并成 IN
ConditionPtr simplify_or(const Condition *node) {
  std::vector<const Condition *> flat;
  collect_same_op(node, ConditionType::OR, flat);

  std::vector<ConditionPtr> children;
  for (const Condition *child : flat) {
    children.push_back(simplify_impl(child));
  }

  // ---- 吸收律：A OR (A AND B) -> A ----
  std::vector<bool> absorbed(children.size(), false);
  for (size_t i = 0; i < children.size(); ++i) {
    if (!children[i]->is_and()) {
      continue;
    }
    for (size_t j = 0; j < children.size(); ++j) {
      if (i == j) {
        continue;
      }
      if (tree_contains(*children[i], *children[j])) {
        absorbed[i] = true;
        break;
      }
    }
  }
  std::vector<ConditionPtr> kept;
  for (size_t i = 0; i < children.size(); ++i) {
    if (!absorbed[i]) {
      kept.push_back(std::move(children[i]));
    }
  }

  // ---- x = 1 OR x = 2 -> x IN (1, 2) ----
  // 只合并同层、同列的 EQ（在 AND 里的 EQ 不能这样合，OR 才可以）
  std::vector<ConditionPtr> merged;
  std::vector<bool> consumed(kept.size(), false);
  for (size_t i = 0; i < kept.size(); ++i) {
    if (consumed[i] || !kept[i]->is_compare()) {
      continue;
    }
    const auto &head = static_cast<const sql::CompareCondition &>(*kept[i]);
    if (head.op() != CompareOp::EQ) {
      continue;
    }
    std::vector<sql::Value> values{head.value()};
    std::vector<size_t> group{i};
    for (size_t j = i + 1; j < kept.size(); ++j) {
      if (consumed[j] || !kept[j]->is_compare()) {
        continue;
      }
      const auto &other = static_cast<const sql::CompareCondition &>(*kept[j]);
      if (other.op() == CompareOp::EQ && other.column() == head.column()) {
        values.push_back(other.value());
        group.push_back(j);
      }
    }
    if (group.size() == 1) {
      continue;
    }
    for (size_t index : group) {
      consumed[index] = true;
    }
    merged.push_back(sql::make_in(head.column(), false, std::move(values)));
  }
  for (size_t i = 0; i < kept.size(); ++i) {
    if (!consumed[i]) {
      merged.push_back(std::move(kept[i]));
    }
  }
  return rebuild(ConditionType::OR, std::move(merged));
}

ConditionPtr simplify_impl(const Condition *node) {
  if (node == nullptr) {
    return nullptr;
  }
  switch (node->type()) {
  case ConditionType::AND:
    return simplify_and(node);
  case ConditionType::OR:
    return simplify_or(node);
  case ConditionType::NOT: {
    const auto &n = static_cast<const sql::NotCondition &>(*node);
    ConditionPtr child = simplify_impl(n.child());
    if (!child) {
      return nullptr;
    }
    if (child->is_not()) { // NOT NOT A -> A
      return static_cast<sql::NotCondition &>(*child).take_child();
    }
    if (child->is_in()) { // NOT (x IN ..) -> x NOT IN ..
      auto &in = static_cast<sql::InCondition &>(*child);
      return sql::make_in(in.column(), !in.is_not_in(), in.values());
    }
    return sql::make_not(std::move(child));
  }
  default:
    return node->clone();
  }
}

} // namespace

// ============================================================
// 对外接口
// ============================================================
sql::ConditionPtr rewrite_condition_tree(const Condition *root) {
  if (root == nullptr) {
    return nullptr;
  }
  ConditionPtr rewritten = pushdown_not_impl(root, false);
  rewritten = flatten_impl(rewritten.get());
  rewritten = fold_constants_impl(rewritten.get());
  rewritten = deduplicate_impl(rewritten.get());
  rewritten = simplify_impl(rewritten.get());
  return rewritten;
}

sql::Query clone_query(const sql::Query &query) {
  return query.visit([](const auto &stmt) -> sql::Query {
    using T = std::decay_t<decltype(stmt)>;
    if constexpr (std::is_same_v<T, sql::SelectQuery>) {
      sql::SelectQuery copy;
      copy.table = stmt.table;
      copy.alias = stmt.alias;
      copy.columns = stmt.columns;
      copy.where = clone_or_null(stmt.where.get());
      copy.group_by = stmt.group_by;
      copy.order_by = stmt.order_by;
      copy.limit = stmt.limit;
      return sql::Query(std::move(copy));
    } else if constexpr (std::is_same_v<T, sql::UpdateQuery>) {
      sql::UpdateQuery copy;
      copy.table = stmt.table;
      copy.assignments = stmt.assignments;
      copy.where = clone_or_null(stmt.where.get());
      return sql::Query(std::move(copy));
    } else if constexpr (std::is_same_v<T, sql::DeleteQuery>) {
      sql::DeleteQuery copy;
      copy.table = stmt.table;
      copy.where = clone_or_null(stmt.where.get());
      return sql::Query(std::move(copy));
    } else {
      // INSERT / DDL / USE：没有条件树，直接拷贝
      return sql::Query(stmt);
    }
  });
}

const Condition *query_where(const sql::Query &query) {
  if (const auto *select = query.select()) {
    return select->where.get();
  }
  if (const auto *update = query.update()) {
    return update->where.get();
  }
  if (const auto *del = query.delete_()) {
    return del->where.get();
  }
  return nullptr;
}

// ============================================================
// QueryRewriter
// ============================================================
std::expected<sql::Query, PlanError>
QueryRewriter::rewrite(const sql::Query &query) {
  if (query.type() == sql::QueryType::UNKNOWN) {
    return std::unexpected(
        PlanError(PlanErrorCode::UNSUPPORTED_QUERY, "unknown query type"));
  }

  sql::Query rewritten = clone_query(query);
  switch (rewritten.type()) {
  case sql::QueryType::SELECT: {
    auto *select = rewritten.select();
    select->where = rewrite_conditions(select->where.get());
    break;
  }
  case sql::QueryType::UPDATE: {
    auto *update = rewritten.update();
    update->where = rewrite_conditions(update->where.get());
    break;
  }
  case sql::QueryType::DELETE: {
    auto *del = rewritten.delete_();
    del->where = rewrite_conditions(del->where.get());
    break;
  }
  default:
    break; // INSERT / DDL / USE：原样返回
  }
  return rewritten;
}

sql::ConditionPtr QueryRewriter::rewrite_conditions(const Condition *root) {
  return rewrite_condition_tree(root);
}

sql::ConditionPtr QueryRewriter::fold_constants(const Condition *cond_root) {
  return fold_constants_impl(cond_root);
}

sql::ConditionPtr QueryRewriter::deduplicate(const Condition *cond_root) {
  return deduplicate_impl(cond_root);
}

sql::ConditionPtr QueryRewriter::pushdown_not(const Condition *cond_root) {
  return pushdown_not_impl(cond_root, false);
}

sql::ConditionPtr QueryRewriter::flattern(const Condition *cond_root) {
  return flatten_impl(cond_root);
}

sql::ConditionPtr QueryRewriter::simplify(const Condition *cond_root) {
  return simplify_impl(cond_root);
}

} // namespace plan
