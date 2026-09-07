// condition.cpp
#include "condition.h"

#include <algorithm>
#include <sstream>
#include <unodered_set>

namespace stmt {

// ============================================================
// ConditionExpr 构造
// ============================================================
ConditionExpr::ConditionExpr(const std::string& column, CompareOp op,
                             sql::Value value)
    : type_(ConditionType::COMPARE),
      column_(column),
      op_(op),
      value_(std::move(value)) {}

ConditionExpr::ConditionExpr(ConditionType type,
                             std::unique_ptr<ConditionExpr> left,
                             std::unique_ptr<ConditionExpr> right)
    : type_(type), left_(std::move(left)), right_(std::move(right)) {}

ConditionExpr::ConditionExpr(ConditionType type,
                             std::unique_ptr<ConditionExpr> child)
    : type_(type), left_(std::move(child)) {}

ConditionExpr::ConditionExpr(ConditionType type, const std::string& column,
                             std::vector<sql::Value> values)
    : type_(type), column_(column), in_values_(std::move(values)) {}

// ----- 静态工厂 -----
ConditionExpr ConditionExpr::make_compare(const std::string& column,
                                          CompareOp op, sql::Value value) {
  return ConditionExpr(column, op, std::move(value));
}

ConditionExpr ConditionExpr::make_and(std::unique_ptr<ConditionExpr> left,
                                      std::unique_ptr<ConditionExpr> right) {
  return ConditionExpr(ConditionType::AND, std::move(left), std::move(right));
}

ConditionExpr ConditionExpr::make_or(std::unique_ptr<ConditionExpr> left,
                                     std::unique_ptr<ConditionExpr> right) {
  return ConditionExpr(ConditionType::OR, std::move(left), std::move(right));
}

ConditionExpr ConditionExpr::make_not(std::unique_ptr<ConditionExpr> child) {
  return ConditionExpr(ConditionType::NOT, std::move(child));
}

ConditionExpr ConditionExpr::make_in(const std::string& column,
                                     std::vector<sql::Value> values) {
  return ConditionExpr(ConditionType::IN, column, std::move(values));
}

// ----- 拷贝 -----
ConditionExpr::ConditionExpr(const ConditionExpr& other)
    : type_(other.type_),
      column_(other.column_),
      op_(other.op_),
      value_(other.value_),
      in_values_(other.in_values_) {
  if (other.left_) left_ = std::make_unique<ConditionExpr>(*other.left_);
  if (other.right_) right_ = std::make_unique<ConditionExpr>(*other.right_);
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

// ----- 赋值 -----
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
// only_primary_key
// ============================================================
bool ConditionExpr::involve_only_primary_key(
    const sql::TableSchema& schema) const {
  auto pk = schema.primary_key_name();
  if ("" == pk) return false;
  return is_only_primary_key(pk);
}

bool ConditionExpr::is_only_primary_key(const std::string& pk) const {
  switch (type_) {
    case ConditionType::COMPARE:
      return column_ == pk;
    case ConditionType::IN:
      return column_ == pk;
    case ConditionType::AND:
      return left_ && right_ && left_->is_only_primary_key(pk) &&
             right_->is_only_primary_key(pk);
    case ConditionType::OR:
      return left_ && right_ && left_->is_only_primary_key(pk) &&
             right_->is_only_primary_key(pk);
    case ConditionType::NOT:
      return left_ && left_->is_only_primary_key(pk);
    default:
      return false;
  }
}

// ============================================================
// to_string
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
      std::string s = column_ + " IN (";
      for (size_t i = 0; i < in_values_.size(); ++i) {
        if (i) s += ", ";
        s += in_values_[i].to_string();
      }
      s += ")";
      return s;
    }
    default:
      return "?";
  }
}

// ============================================================
// match row
// ============================================================
bool ConditionExpr::match_row(const sql::Row& row,
                              const sql::TableSchema& schema) const {
  switch (type_) {
    case ConditionType::COMPARE: {
      auto row_val = get_column_value(row, schema);
      return compare_value(op_, row_val, value_);
    }
    case ConditionType::IN: {
      auto row_val = get_column_value(row, schema);
      return match_in_expr(row_val);
    }
    case ConditionType::AND:
      return left_->match_row(row, schema) && right_->match_row(row, schema);
    case ConditionType::OR:
      return left_->match_row(row, schema) || right_->match_row(row, schema);
    case ConditionType::NOT:
      return !left_->match_row(row, schema);
    default:
      return false;
  }
}

sql::Value ConditionExpr::get_column_value(
    const sql::Row& row, const sql::TableSchema& schema) const {
  int idx = schema.column_index(column_);
  if (idx < 0 || idx >= static_cast<int>(row.size())) return sql::Value();

  return row[idx];
}

bool ConditionExpr::match_in_expr(const sql::Value& row_val) const {
  if (row_val.is_null() || row_val.type() != in_values_[0].type()) return false;
  return std::find(in_values_.begin(), in_values_.end(), row_val) !=
         in_values_.end();
}

// ============================================================
// 辅助：NOT 下推（De Morgan 定律）
// 将 NOT 推到叶子节点，把 NOT(id>10) 变成 id<=10
// ============================================================
// root tree: push_down_not(expr, false)
// child of not: push_down_not(expr, true)
std::unique_ptr<ConditionExpr> ConditionExpr::pushdown_not(
    std::unique_ptr<ConditionExpr> expr, bool need_flip) {
  if (!expr) return nullptr;

  if (expr->is_not()) {
    return pushdown_not(std::move(expr->move_left()), !need_flip);
  }

  if (expr->is_and()) {
    auto new_left = pushdown_not(std::move(expr->move_left()), need_flip);
    auto new_right = pushdown_not(std::move(expr->move_right()), need_flip);
    if (need_flip) {
      return std::make_unique<ConditionExpr>(
          ConditionType::OR, std::move(new_left), std::move(new_right));
    } else {
      return std::make_unique<ConditionExpr>(
          ConditionType::AND, std::move(new_left), std::move(new_right));
    }
  }
  if (expr->is_or()) {
    auto new_left = pushdown_not(std::move(expr->move_left()), need_flip);
    auto new_right = pushdown_not(std::move(expr->move_right()), need_flip);
    if (need_flip) {
      return std::make_unique<ConditionExpr>(
          ConditionType::AND, std::move(new_left), std::move(new_right));
    } else {
      return std::make_unique<ConditionExpr>(
          ConditionType::OR, std::move(new_left), std::move(new_right));
    }
  }

  if (expr->is_leaf()) {
    if (need_flip) {
      // try flip, else return with "NOT"
      if (expr->is_compare()) {
        auto flip_op = flip_compare_op(expr->op());
        if (flip_op == CompareOp::UNKNOWN) {
          return std::make_unique<ConditionExpr>(ConditionType::NOT,
                                                 std::move(expr));
        } else {
          return std::make_unique<ConditionExpr>(expr->column(), flip_op,
                                                 expr->value());
        }

      } else if (expr->is_in()) {
        // IN 无法翻转，包装 NOT
        return std::make_unique<ConditionExpr>(ConditionType::NOT,
                                               std::move(expr));
      } else {
        return std::make_unique<ConditionExpr>(ConditionType::NOT,
                                               std::move(expr));
      }
    } else {
      // 不需要翻转
      return expr;
    }
  }

  if (need_flip) {
    return std::make_unique<ConditionExpr>(ConditionType::NOT, std::move(expr));
  } else {
    return expr;
  }
}

// 规范化primary key为左闭右开区间, 消除 > and <=
std::unique_ptr<ConditionExpr> normalize_boundary(
    std::unique_ptr<ConditionExpr> expr, const std::string& primary_key) {
  if (!expr) return nullptr;

  if (expr->is_internal()) {
    // and, or, not
    auto new_left = normalize_boundary(expr->move_left(), primary_key);
    auto new_right = normalize_boundary(expr->move_right(), primary_key);
    return std::make_unique<ConditionExpr>(expr->type(), std::move(new_left),
                                           std::move(new_right));
  }

  if (expr->is_compare() && expr->column() == primary_key) {
    switch (expr->op()) {
      case CompareOp::GT: {
        // 如果值是整数，可以直接用 value+1 转为 >=
        if (expr->value().is_int()) {
          return std::make_unique<ConditionExpr>(
              expr->column(), CompareOp::GE,
              sql::Value(expr->value().int_val() + 1));
        }
        // 否则展开为 AND 组合
        auto ge_expr = std::make_unique<ConditionExpr>(
            expr.column(), CompareOp::GE, expr->value());
        auto ne_expr = std::make_unique<ConditionExpr>(
            expr.column(), CompareOp::NE, expr->value());
        return std::make_unique<ConditionExpr>(
            ConditionType::AND, std::move(ge_expr), std::move(ne_expr));
      }
      case CompareOp::LE: {
        // 如果值是整数，可以直接用 value-1 转为 <
        if (expr->value().is_int()) {
          // id <= 20 → id < 21 (对于整数)
          return std::make_unique<ConditionExpr>(
              expr->column(), CompareOp::LT,
              sql::Value(expr->value().int_val() + 1));
        }
        // 否则展开为 OR 组合
        auto lt_expr = std::make_unique<ConditionExpr>(
            expr->column(), CompareOp::LT, expr->value());
        auto eq_expr = std::make_unique<ConditionExpr>(
            expr->column(), CompareOp::EQ, expr->value());
        return std::make_unique<ConditionExpr>(
            ConditionType::OR, std::move(lt_expr), std::move(eq_expr));
      }
      default:
        // EQ, GE, LT, NE, LIKE, IS_NULL, IS_NOT_NULL 保持不变
        return expr;
    }
  }
  // ---- IN 节点或非主键列：保持不变 ----
  return expr;
}

bool ConditionExpr::is_true_expr(const ConditionExpr& expr) {
  // 用 1=1 表示 true
  if (expr.is_compare() && expr.column() == "1" && expr.op() == CompareOp::EQ &&
      expr.value().is_int() && expr.value().int_val() == 1) {
    return true;
  }
  return false;
}

bool ConditionExpr::is_false_expr(const ConditionExpr& expr) {
  // 用 1=0 表示 false
  if (expr.is_compare() && expr.column() == "1" && expr.op() == CompareOp::EQ &&
      expr.value().is_int() && expr.value().int_val() == 0) {
    return true;
  }
  return false;
}

std::unique_ptr<ConditionExpr> make_true_expr() {
  return std::make_unique<ConditionExpr>("1", CompareOp::EQ, sql::Value(1));
}

std::unique_ptr<ConditionExpr> make_false_expr() {
  return std::make_unique<ConditionExpr>("1", CompareOp::EQ, sql::Value(0));
}

// ============================================================
// 辅助：收集 OR 节点中的所有 EQ 条件
// ============================================================
// 扁平化 OR 树，收集所有叶子条件
void ConditionExpr::flatten_or(const ConditionExpr* expr,
                               std::vector<const ConditionExpr*>& leaves) {
  if (!expr) return;
  if (expr->is_or()) {
    flatten_or(expr->left(), leaves);
    flatten_or(expr->right(), leaves);
  } else {
    leaves.push_back(expr);
  }
}

void ConditionExpr::group_eq_by_column(
    const std::vector<const ConditionExpr*>& leaves,
    std::unordered_map<std::string, std::vector<const ConditionExpr*>>&
        groups) {
  for (auto* leaf : leaves) {
    // 只收集 EQ, 非叶子节点，非EQ不收集
    if (leaf->is_compare() && leaf->op() == CompareOp::EQ) {
      groups[leaf->column()].push_back(leaf);
    }
  }
}

bool ConditionExpr::contains_expr(const ConditionExpr& container,
                                  const ConditionExpr& target) {
  if (container.type() == target.type() &&
      container.to_string() == target.to_string())
    return true;
  if (container.is_and() || container.is_or()) {
    if (container.left() && contains_expr(*container.left(), target))
      return true;
    if (container.right() && contains_expr(*container.right(), target))
      return true;
  }
  return false;
}

std::unique_ptr<ConditionExpr> ConditionExpr::simplify_or(
    std::unique_ptr<ConditionExpr> expr_or) {
  if (!expr_or) return nullptr;

  if (!expr_or->s_or()) return expr_or;  // (expr_or.)

  auto left = expr_or->move_left();
  auto right = expr_or->move_right();

  // 1. 常量折叠
  if (is_true_expr(*left) || is_true_expr(*right)) {
    return make_true_expr();
  }
  if (is_false_expr(*left)) return right;
  if (is_false_expr(*right)) return left;

  // 2. 去重
  if (left->type() == right->type() &&
      left->to_string() == right->to_string()) {
    return left;
  }

  // 3. 吸收律: A OR (A AND B) → A
  if (right->is_and() && contains_expr(*right, *left)) {
    return left;
  }
  if (left->is_and() && contains_expr(*left, *right)) {
    return right;
  }

  if (left->is_compare() && right->is_compare() &&
      left->column() == right->column()) {
    auto merged = merge_or_compare_conditions(*left, *right);
    if (merged) {
      return merged;
    }
  }

  // 4. 扁平化 OR，按列分组合并 EQ → IN
  std::vector<const ConditionExpr*> leaves;
  // 先收集当前 OR 的所有叶子
  flatten_or(left.get(), leaves);
  flatten_or(right.get(), leaves);

  std::unordered_map<std::string, std::vector<const ConditionExpr*>> eq_groups;
  group_eq_by_column(leaves, eq_groups);

  return build_optimized_or(leaves, eq_groups);
}

std::unique_ptr<ConditionExpr> ConditionExpr::build_optimized_or(
    const std::vector<const ConditionExpr*>& leaves,
    const std::unordered_map<std::string, std::vector<const ConditionExpr*>>&
        eq_groups) {
  std::unique_ptr<ConditionExpr> result;
  std::unordered_set<const ConditionExpr*> consumed;

  for (const auto& [column, eqs] : eq_groups) {
    if (eqs.size() >= 2) {
      std::vector<sql::Value> values;
      for (auto* eq : eqs) {
        values.push_back(eq->value());
        consumed.insert(eq);
      }
      auto in_expr = std::make_unique<ConditionExpr>(ConditionType::IN, column,
                                                     std::move(values));
      if (!result) {
        result = std::move(in_expr);
      } else {
        result = std::make_unique<ConditionExpr>(
            ConditionType::OR, std::move(result), std::move(in_expr));
      }
    }
  }

  // 添加未被合并的叶子
  for (auto* leaf : leaves) {
    if (consumed.find(leaf) == consumed.end()) {
      auto cloned = leaf->clone();
      if (!result) {
        result = std::move(cloned);
      } else {
        result = std::make_unique<ConditionExpr>(
            ConditionType::OR, std::move(result), std::move(cloned));
      }
    }
  }

  return result;
}
// ============================================================
// 合并两个比较条件（AND/OR 语义）
// ============================================================
std::unique_ptr<ConditionExpr> ConditionExpr::merge_and_compare_conditions(
    const ConditionExpr& a, const ConditionExpr& b) {
  // 只处理同一列且都是 COMPARE
  if (a.column() != b.column() || !a.is_compare() || !b.is_compare()) {
    return nullptr;
  }

  auto va = a.value();
  auto vb = b.value();
  // 定义操作符的“严格程度”用于合并
  // 对于 AND：取更严格的条件

  // 这里只做几个常见合并
  // id >= 5 AND id > 4 → id >= 5
  if ((a.op() == CompareOp::GE || a.op() == CompareOp::GT) &&
      b.op() == CompareOp::GT && va > vb) {
    return a->clone();
  }
  // id > 5 AND id >= 5 → id > 5
  if ((a.op() == CompareOp::GE || a.op() == CompareOp::GT) &&
      b.op() == CompareOp::GE && va >= vb) {
    return a->clone();
  }
  // symemtric
  if ((b.op() == CompareOp::GE || b.op() == CompareOp::GT) &&
      a.op() == CompareOp::GT && vb > va) {
    return b->clone();
  }
  // id > 5 AND id >= 5 → id > 5
  if ((b.op() == CompareOp::GE || b.op() == CompareOp::GT) &&
      a.op() == CompareOp::GE && vb >= va) {
    return b->clone();
  }

  // id < 5 AND id <= 5 → id < 5
  if ((a.op() == CompareOp::LT || a.op() == CompareOp::LE) &&
      b.op() == CompareOp::LE && va <= vb) {
    return a->clone();
  }
  // id <= 4 AND id < 5 → id < 4
  if ((a.op() == CompareOp::LT || a.op() == CompareOp::LE) &&
      b.op() == CompareOp::LT && va < vb) {
    return a->clone();
  }
  // symemtric
  if ((b.op() == CompareOp::LT || b.op() == CompareOp::LE) &&
      a.op() == CompareOp::LE && vb <= va) {
    return b->clone();
  }
  // id <= 4 AND id < 5 → id < 4
  if ((b.op() == CompareOp::LT || b.op() == CompareOp::LE) &&
      a.op() == CompareOp::LT && vb < va) {
    return b->clone();
  }

  return nullptr;
}

std::unique_ptr<ConditionExpr> merge_or_compare_conditions(
    const ConditionExpr& a, const ConditionExpr& b) {
  // 只处理同一列且都是 COMPARE
  if (a.column() != b.column() || !a.is_compare() || !b.is_compare()) {
    return nullptr;
  }

  auto va = a.value();
  auto vb = b.value();
  // OR 合并
  if ((a.op() == CompareOp::GT || a.op() == CompareOp::GE) &&
      (b.op() == CompareOp::GT || b.op() == CompareOp::GE)) {
    auto less = std::min(va, vb);
    CompareOp op = CompareOp::GT;
    if ((a.op() == CompareOp::GE && a.value() == less) ||
        (b.op() == CompareOp::GE && b.value() == less)) {
      op = CompareOp::GE;
    }
    return std::make_unique<ConditionExpr>(a.column(), op, less);
  }

  if ((a.op() == CompareOp::LT || a.op() == CompareOp::LE) &&
      (b.op() == CompareOp::LT || b.op() == CompareOp::LE)) {
    auto greater = std::max(va, vb);
    CompareOp op = CompareOp::LT;
    if ((a.op() == CompareOp::LE && a.value() == greater) ||
        (b.op() == CompareOp::LE && b.value() == greater)) {
      op = CompareOp::LE;
    }
    return std::make_unique<ConditionExpr>(a.column(), op, greater);
  }

  return nullptr;
}

std::unique_ptr<ConditionExpr> ConditionExpr::simplify_and(
    std::unique_ptr<ConditionExpr> expr_and) {
  if(!expr_and) return nullptr;

  if (!expr_and->is_and()) return expr_and;  // (expr_and.)

  auto left = expr_and->move_left();
  auto right = expr_and->move_right();
  if (!left || !right) return nullptr;

  // 1. 常量折叠
  if (is_false_expr(*left) || is_false_expr(*right)) {
    return make_false_expr();
  }
  if (is_true_expr(*left)) return right->clone();
  if (is_true_expr(*right)) return left->clone();

  // 2. 去重：如果两边相同，返回一侧
  if (left->type() == right->type() && left->to_string() == right->to_string()) {
    return left;
  }

  // 3. 吸收律: A AND (A OR B) → A
  if (right->is_or() && contains_expr(*right, *left)) {
    return left;
  }
  if (left->is_or() && contains_expr(*left, *right)) {
    return right;
  }

  // 4. 合并范围条件（同列）
  if (left->is_compare() && right->is_compare() &&
      left->column() == right->column()) {
    auto merged = merge_and_compare_conditions(*left, *right);
    if (merged) return merged;
  }

  // 5. 构建新的 AND
  return std::make_unique<ConditionExpr>(ConditionType::AND, std::move(left), std::move(right));
}

std::unique_ptr<ConditionExpr> ConditionExpr::simplify_not(
    std::unique_ptr<ConditionExpr> expr_not) {
  if(!expr_not) return nullptr;

  if (!expr_not.is_not()) return expr_not;

  auto* child = expr_not.left();

  if (!child) return nullptr;

  // NOT(NOT(A)) → A
  if (child->is_not()) {
    return expr_not->move_left()->move_left();
  }

  // NOT(true) → false, NOT(false) → true
  if (is_true_expr(*child)) return make_false_expr();
  if (is_false_expr(*child)) return make_true_expr();

  return expr_not;
}

std::unique_ptr<ConditionExpr> ConditionExpr::simplify(
    std::unique_ptr<ConditionExpr> expr) {
  if(!expr) return nullptr;

  // 1. 叶子节点：直接克隆（常量折叠可以在这里做）
  if (expr->is_leaf()) {
    return expr;
  }

  // 2. 递归化简子节点
  auto new_left = expr->left()  ? simplify(expr->move_left()) : nullptr;
  auto new_right = expr->right() ? simplify(expr->move_right()) : nullptr;

  // 3. 根据类型化简
  if (expr->is_and()) {
    return simplify_and(std::move(expr));
  }
  if (expr->is_or()) {
    return simplify_or(std::move(expr));
  }
  if (expr->is_not()) {
    return simplify_not(std::move(expr));
  }

  // 如果子节点没有变化，返回原表达式
  //if (new_left == expr.left() && new_right == expr.right()) {
  //  return expr;
  //}
  return std::make_unique<ConditionExpr>(expr.type(), std::move(new_left),
                                         std::move(new_right));
}

std::unique_ptr<ConditionExpr> ConditionExpr::optimize(
    std::unique_ptr<ConditionExpr> expr, sql::TableSchema& schema) {
  if (!expr) return nullptr;
  expr = simplify(std::move(expr));
  expr = pushdown_not(std::move(expr));
  expr = normalize_boundary(std::move(expr), schema.primary_key_name());
  expr = simplify(std::move(expr));
  return expr;
}

ConditionExtractResult extract_primary_key_conditions(
    const sql::TableSchema& schema) const;
{
  auto optimized = optimize(schema);

  auto result =  do_extract_pk(optimized, schema.primary_key_name());
  result.pk_cond = optimize(std::move(result.pk_cond), schema);
  result.remaining = optimize(std::move(result.remaining), schema);

  return result;
}


ConditionExtractResult ConditionExpr::do_extract_pk(
    std::unique_ptr<ConditionExpr> expr, const std::string& primary_key) {

  ConditionExtractResult result;
  if (!expr) return result;

  // ============================================================
  // 叶子节点
  // ============================================================
  if (expr->is_leaf()) {
    // ---- 主键 COMPARE ----
    if (expr->is_compare() && expr->column() == primary_key) {
      // 检查是否是可索引的操作符
      if (is_indexable_compare_op(expr->op())) {
        result.pk_cond = std::move(expr);
      } else {
        // NE, LIKE 等不可索引，放入 remaining
        result.remaining = std::move(expr);
      }
      return result;
    }

    // ---- 主键 IN ----
    if (expr->is_in() && expr->column() == primary_key) {
      result.pk_cond = std::move(expr);
      return result;
    }

    // ---- 非主键叶子 ----
    result.remaining = std::move(expr);
    return result;
  }

  // ============================================================
  // AND 节点
  // ============================================================
  if (expr->is_and()) {
    auto left_result = do_extract_pk(expr->move_left(), schema);
    auto right_result = do_extract_pk(expr->move_right(), schema);

    // 合并 pk_cond（AND 语义）
    result.pk_cond = merge_and_expr(std::move(left_result.pk_cond),
                                    std::move(right_result.pk_cond));

    // 合并 remaining（AND 语义）
    result.remaining = merge_and_expr(std::move(left_result.remaining),
                                      std::move(right_result.remaining));

    return result;
  }

  // ============================================================
  // OR 节点
  // ============================================================
  if (expr->is_or()) {
    auto left_result = do_extract_pk(expr->move_left(), schema);
    auto right_result = do_extract_pk(expr->move_right(), schema);

    // 检查两侧是否都是纯主键条件（即没有 remaining）
    bool left_is_pk_only = !left_result.remaining;
    bool right_is_pk_only = !right_result.remaining;

    if (left_is_pk_only && right_is_pk_only) {
      // 两侧都是纯主键 → 合并为 OR（主键条件）
      result.pk_cond = merge_or_expr(std::move(left_result.pk_cond),
                                     std::move(right_result.pk_cond));
    } else {
      // 任一侧有非主键条件 → 整个 OR 放入 remaining
      // 重新构建 OR 表达式
      auto left_expr = rebuild_from_result(left_result);
      auto right_expr = rebuild_from_result(right_result);
      result.remaining = std::make_unique<ConditionExpr>(
          ConditionType::OR, std::move(left_expr), std::move(right_expr));
    }

    return result;
  }

  // ============================================================
  // NOT 节点
  // ============================================================
  if (expr->is_not()) {
    // 检查子节点是否是完全的主键条件
    auto child_result = do_extract_pk(expr->move_left(), schema);

    if (child_result.pk_cond && !child_result.remaining) {
      // 子节点是纯主键条件
      // NOT(主键) 通常无法下推
      result.pk_cond = std::make_unique<ConditionExpr>(
          ConditionType::NOT, std::move(child_result.pk_cond));
    } else if (child_result.remaining && !child_result.pk_cond) {
      // 子节点是纯剩余条件
      result.remaining = std::make_unique<ConditionExpr>(
          ConditionType::NOT, std::move(child_result.remaining));
    } else {
      // 子节点混合，整个 NOT 保留
      // 需要重新构建
      auto child_expr = rebuild_from_result(child_result);
      result.remaining = std::make_unique<ConditionExpr>(ConditionType::NOT,
                                                         std::move(child_expr));
    }

    return result;
  }

  // 其他未知节点 → 放入 remaining
  result.remaining = std::move(expr);
  return result;
}

// ============================================================
// 辅助：合并两个表达式（AND 语义）
// ============================================================
std::unique_ptr<ConditionExpr> merge_and_expr(
    std::unique_ptr<ConditionExpr> left, std::unique_ptr<ConditionExpr> right) {
  if (!left && !right) return nullptr;
  if (!left) return right;
  if (!right) return left;

  // 如果一边是 AND，可以扁平化，也可以简单嵌套
  return std::make_unique<ConditionExpr>(ConditionType::AND, std::move(left),
                                         std::move(right));
}

// ============================================================
// 辅助：合并两个表达式（OR 语义）
// ============================================================
std::unique_ptr<ConditionExpr> merge_or_expr(
    std::unique_ptr<ConditionExpr> left, std::unique_ptr<ConditionExpr> right) {
  if (!left && !right) return nullptr;
  if (!left) return right;
  if (!right) return left;

  return std::make_unique<ConditionExpr>(ConditionType::OR, std::move(left),
                                         std::move(right));
}

// ============================================================
// 辅助：从提取结果重建表达式
// ============================================================
std::unique_ptr<ConditionExpr> rebuild_from_result(
    const ConditionExtractResult& result) {
  // pk_cond AND remaining
  if (result.pk_cond && result.remaining) {
    return std::make_unique<ConditionExpr>(
        ConditionType::AND, std::move(result.pk_cond), std::move(result.remaining));
  }
  if (result.pk_cond) {
    return result.pk_cond;
  }
  if (result.remaining) {
    return result.remaining;
  }
  return nullptr;
}



}  // namespace query