// condition.cpp
#include "condition.h"

#include <algorithm>
#include <sstream>

namespace query {

// ============================================================
// PrimaryKeyFragment
// ============================================================
bool PrimaryKeyFragment::is_valid() const {
  if (is_point_set) return !points.empty();

  // 范围必须有至少一个边界，且如果两个边界都存在，start 必须 < end
  if (start || end) {
    if (start && end && *start >= *end) return false;
    return true;
  }

  // 既不是点集也没有范围边界：无效条件
  // 注意：全表扫描由 PrimaryKeyCondition::empty() 表示，不由 Fragment 表示
  return false;
}

bool PrimaryKeyFragment::operator==(const PrimaryKeyFragment& other) const {
  return is_point_set == other.is_point_set && points == other.points &&
         start == other.start && end == other.end;
}

std::string PrimaryKeyFragment::to_string() const {
  if (is_point_set) {
    std::string s = "points{";
    for (size_t i = 0; i < points.size(); ++i) {
      if (i) s += ",";
      s += points[i].to_string();
    }
    s += "}";
    return s;
  }
  std::string s = "range[";
  s += start ? start->to_string() : "-∞";
  s += ", ";
  s += end ? end->to_string() : "+∞";
  s += ")";
  return s;
}

// ============================================================
// PrimaryKeyCondition - 辅助函数
// ============================================================

// 合并两个 Fragment（AND 语义），返回 nullopt 表示矛盾
std::optional<PrimaryKeyFragment> PrimaryKeyFragment::merge_fragments_and(
    const PrimaryKeyFragment& a, const PrimaryKeyFragment& b) {
  // 点集 ∩ 点集 -> 交集
  if (a.is_point_set && b.is_point_set) {
    PrimaryKeyFragment merged;
    merged.is_point_set = true;
    for (const auto& val : a.points) {
      if (std::find(b.points.begin(), b.points.end(), val) != b.points.end()) {
        merged.points.push_back(val);
      }
    }
    if (merged.points.empty()) return std::nullopt;
    return merged;
  }

  // 范围 ∩ 范围 -> 取交集
  if (!a.is_point_set && !b.is_point_set) {
    PrimaryKeyFragment merged;
    if (a.start && b.start) {
      merged.start = (*a.start > *b.start) ? a.start : b.start;
    } else if (a.start) {
      merged.start = a.start;
    } else if (b.start) {
      merged.start = b.start;
    }
    if (a.end && b.end) {
      merged.end = (*a.end < *b.end) ? a.end : b.end;
    } else if (a.end) {
      merged.end = a.end;
    } else if (b.end) {
      merged.end = b.end;
    }
    if (!merged.is_valid()) return std::nullopt;
    return merged;
  }

  // 点集 ∩ 范围 -> 筛选点集
  PrimaryKeyFragment merged;
  merged.is_point_set = true;

  const auto* point_set = a.is_point_set ? &a : &b;
  const auto* range = a.is_point_set ? &b : &a;

  for (const auto& val : point_set->points) {
    bool in_range = true;
    if (range->start && val < *range->start) in_range = false;
    if (range->end && val >= *range->end) in_range = false;
    if (in_range) merged.points.push_back(val);
  }

  if (merged.points.empty()) return std::nullopt;
  return merged;
}

// 检查两个范围是否可以合并
std::optional<PrimaryKeyFragment> PrimaryKeyFragment::can_merge_ranges_or(
    const PrimaryKeyFragment& a, const PrimaryKeyFragment& b) {
  if (a.is_point_set || b.is_point_set) return std::nullopt;

  // 确定哪个范围在左边
  const PrimaryKeyFragment* left = &a;
  const PrimaryKeyFragment* right = &b;

  // 按 start 排序
  if (left->start && right->start && *left->start > *right->start) {
    std::swap(left, right);
  } else if (!left->start && right->start) {
    // left 无 start（-∞），在左边
    // 保持原样
  } else if (left->start && !right->start) {
    std::swap(left, right);
  }

  // 检查是否重叠或相邻
  if (left->end && right->start && *left->end < *right->start) {
    return std::nullopt;  // 不重叠，中间有间隙
  }

  // 可以合并
  auto merged = *left;
  if (right->start && (!merged.start || *right->start < *merged.start)) {
    merged.start = right->start;
  }
  if (right->end && (!merged.end || *right->end > *merged.end)) {
    merged.end = right->end;
  }
  return merged;
}

std::optional<PrimaryKeyFragment>
PrimaryKeyFragment::merge_fragments_or_point_set(const PrimaryKeyFragment& a,
                                                 const PrimaryKeyFragment& b) {
  if (a.is_point_set && b.is_point_set) {
    // merge point set
    PrimaryKeyFragment merged;
    merged.is_point_set = true;
    auto all_points = a.points;
    all_points.insert(all_points.end(), b.points.begin(), b.points.end());
    std::sort(all_points.begin(), all_points.end());
    all_points.erase(std::unique(all_points.begin(), all_points.end()),
                     all_points.end());
    merged.points = all_points;
    if (merged.points.empty()) return std::nullopt;
    return merged;
  }
  return std::nullopt;
}

std::optional<PrimaryKeyFragment> PrimaryKeyFragment::can_merge_fragments_or(
    const PrimaryKeyFragment& a, const PrimaryKeyFragment& b) {
  if (a.is_all() || b.is_all()) {
    return PrimaryKeyFragment{};
  }
  if (a.is_point_set && b.is_point_set) {
    return merge_fragments_or_point_set(a, b);
  }
  if (a.is_range() && b.is_range()) {
    auto merged = can_merge_ranges_or(a, b);

    if (!merged) return std::nullopt;  // unmergeable
    return merged;
  }
  if (a.is_point_set_only() && b.is_range()) {
    return std::nullopt;  // unmergeable
  }
  if (a.is_range() && b.is_point_set_only()) {
    return std::nullopt;  // unmergeable
  }
  return std::nullopt;
}

std::vector<PrimaryKeyFragment> PrimaryKeyFragment::merge_vector_ranges_or(
    std::vector<PrimaryKeyFragment>& ranges) {
  if (ranges.size() < 2) return ranges;

  // 按 start 排序（无 start 的排在前面，表示 -∞）
  std::sort(ranges.begin(), ranges.end(),
            [](const PrimaryKeyFragment& a, const PrimaryKeyFragment& b) {
              // 无 start（-∞）排在前面
              if (!a.start && !b.start) {
                // 都无 start，按 end 排序
                if (a.end && b.end) return *a.end < *b.end;
                return a.end.has_value() < b.end.has_value();
              }
              if (!a.start) return true;
              if (!b.start) return false;
              return *a.start < *b.start;
            });

  // 合并重叠的范围
  std::vector<PrimaryKeyFragment> merged_ranges;
  for (const auto& range : ranges) {
    // 无效片段跳过（防御性）
    if (!range.is_valid()) continue;

    if (merged_ranges.empty()) {
      merged_ranges.push_back(range);
      continue;
    }

    auto& last = merged_ranges.back();
    // 检查是否与最后一个重叠或相邻
    // 重叠条件：last 无 end（到 +∞），或 range 无 start（从 -∞），
    // 或 range.start <last.end
    if(!last.end ){
      if(!last.start){
        return {PrimaryKeyFragment{}}; // -∞ +∞
      }else{
        break; // 后面都被+∞覆盖，前面的start永远小于后面的start， 所以没必要检查了
      }
    }
    else{ //last.end != null
      if(!range.end){
        if(*range.start < *last.end){
          //overlap
          last.end = std::nullopt;
          if(!last.start){
            return {PrimaryKeyFragment{}}; //扩展到 -∞ +∞
          }else{
            break; // 后面都被+∞覆盖，前面的start永远小于后面的start， 所以没必要检查了
          }
        }else{
          //no overlap
          merged_ranges.push_back(range);
          break; // 因为range的end覆盖无穷远了，所以后面都无必要检查了
        }
      }else{ // range.end != null
        if(*range.start < *last.end){
          //overlap
          if(*range.end > *last.end){
            last.end = range.end;
            continue;
          }
        }else{
          //no overlap
          merged_ranges.push_back(range);
          continue;
        }
      }
    }
  }

  return merged_ranges;
}

std::optional<PrimaryKeyFragment> PrimaryKeyFragment::merge_vector_point_set_or(
    const std::vector<PrimaryKeyFragment>& fragments) {
  std::vector<sql::Value> all_points;

  for (const auto& frag : fragments) {
    if (frag.is_point_set) {
      all_points.insert(all_points.end(), frag.points.begin(),
                        frag.points.end());
    }
  }

  // ---- 合并点集 ----
  if (!all_points.empty()) {
    std::sort(all_points.begin(), all_points.end());
    all_points.erase(std::unique(all_points.begin(), all_points.end()),
                     all_points.end());
    PrimaryKeyFragment point_frag;
    point_frag.is_point_set = true;
    point_frag.points = all_points;
    return point_frag;
  }
  return std::nullopt;
}

// ============================================================
// PrimaryKeyCondition
// ============================================================

std::optional<PrimaryKeyCondition> PrimaryKeyCondition::merge_and(
    const PrimaryKeyCondition& a, const PrimaryKeyCondition& b) {
  // 如果两边都是空的，说明没有主键条件
  if (a.empty() && b.empty()) {
    return PrimaryKeyCondition{};
  }

  // 如果一边为空，返回另一边
  if (a.empty()) return b;
  if (b.empty()) return a;

  // 简单情况：两边都是单片段
  if (a.is_single() && b.is_single()) {
    auto merged_frag =
        PrimaryKeyFragment::merge_fragments_and(a.fragments[0], b.fragments[0]);
    if (!merged_frag) {
      return std::nullopt;  // 矛盾条件,调用者要报错
    }
    PrimaryKeyCondition result;
    result.fragments.push_back(*merged_frag);
    return result;
  }

  // 复杂情况：至少一边是多片段
  // 计算笛卡尔积
  PrimaryKeyCondition result;
  const auto* left = &a;
  const auto* right = &b;

  for (const auto& lf : left->fragments) {
    for (const auto& rf : right->fragments) {
      auto merged = PrimaryKeyFragment::merge_fragments_and(lf, rf);
      if (merged) {
        result.fragments.push_back(*merged);
      }
    }
  }

  if (result.empty()) {
    return std::nullopt;  // 所有组合都矛盾
  }

  return simplify(result);
}

PrimaryKeyCondition PrimaryKeyCondition::merge_or(
    const PrimaryKeyCondition& a, const PrimaryKeyCondition& b) {
  PrimaryKeyCondition result;
  result.fragments = a.fragments;
  result.fragments.insert(result.fragments.end(), b.fragments.begin(),
                          b.fragments.end());
  return simplify(result);
}

PrimaryKeyCondition PrimaryKeyCondition::simplify(
    const PrimaryKeyCondition& cond) {
  if (cond.fragments.size() <= 1) return cond;

  // 分离点集和范围
  std::vector<PrimaryKeyFragment> all_points;
  std::vector<PrimaryKeyFragment> ranges;

  for (const auto& frag : cond.fragments) {
    if (frag.is_point_set) {
      all_points.push_back(frag);
    } else {
      ranges.push_back(frag);
    }
  }

  PrimaryKeyCondition result;
  // ---- 合并点集 ----
  if (!all_points.empty()) {
    auto merged_points =
        PrimaryKeyFragment::merge_vector_point_set_or(all_points);
    if (merged_points) {
      result.fragments.push_back(*merged_points);
    }
  }

  // ---- 合并范围） ----
  auto merged_ranges = PrimaryKeyFragment::merge_vector_ranges_or(ranges);
  if(merged_ranges.size()== 1 && merged_ranges[0].is_all()){
    //出现了全部覆盖情况，只能全盘扫描
    result.fragments.clear();
    result.fragments.push_back(merged_ranges[0]);
  }else{
    result.fragments.insert(result.fragments.end(), merged_ranges.begin(),
                            merged_ranges.end());
  }


  return result;
}

std::string PrimaryKeyCondition::to_string() const {
  std::string s = "PKCond{";
  for (size_t i = 0; i < fragments.size(); ++i) {
    if (i) s += " OR ";
    s += fragments[i].to_string();
  }
  s += "}";
  return s;
}

// ============================================================
// ScanSpec
// ============================================================
std::string ScanSpec::to_string() const {
  std::string s = "ScanSpec{";
  s += pk_cond.to_string();
  if (order_by_pk) s += ", order_by=" + std::string(ascending ? "ASC" : "DESC");
  if (limit) s += ", limit=" + std::to_string(limit);
  s += "}";
  return s;
}

// ============================================================
// ConditionExpr 构造
// ============================================================
ConditionExpr::ConditionExpr(const std::string& column, CompareOp op,
                             const sql::Value& value)
    : type_(ConditionType::COMPARE), column_(column), op_(op), value_(value) {}

ConditionExpr::ConditionExpr(ConditionType type,
                             std::unique_ptr<ConditionExpr> left,
                             std::unique_ptr<ConditionExpr> right)
    : type_(type), left_(std::move(left)), right_(std::move(right)) {}

ConditionExpr::ConditionExpr(ConditionType type,
                             std::unique_ptr<ConditionExpr> child)
    : type_(type), left_(std::move(child)) {}

ConditionExpr::ConditionExpr(ConditionType type, const std::string& column,
                             std::vector<sql::Value> values)
    : type_(type),
      column_(column),
      in_values_(std::move(values)) {}

// ----- 静态工厂 -----
ConditionExpr ConditionExpr::make_compare(const std::string& column,
                                          CompareOp op,
                                          const sql::Value& value) {
  return ConditionExpr(column, op, value);
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
// matches
// ============================================================
bool ConditionExpr::match(const sql::Row& row,
                            const sql::TableSchema& schema) const {
  switch (type_) {
    case ConditionType::COMPARE:
    case ConditionType::IN:
      return matches_compare(row, schema);
    case ConditionType::AND:
      return left_ && right_ && left_->match(row, schema) &&
             right_->match(row, schema);
    case ConditionType::OR:
      return left_ && right_ &&
             (left_->match(row, schema) || right_->match(row, schema));
    case ConditionType::NOT:
      return left_ && !left_->match(row, schema);
    default:
      return false;
  }
}

bool ConditionExpr::matches_compare(const sql::Row& row,
                                    const sql::TableSchema& schema) const {
  int idx = schema.column_index(column_);
  if (idx < 0 || idx >= static_cast<int>(row.size())) return false;
  return compare_values(row[idx]);
}

bool ConditionExpr::compare_in_values(const sql::Value& row_val) const {
  if(row_val.type() != value_.type()) return false;
  auto it = std::find(in_values_.begin(), in_values_.end(), row_val);
  return it != in_values_.end();
}

bool ConditionExpr::compare_values(const sql::Value& row_val) const {
  if (row_val.is_null()) {
    return op_ == CompareOp::IS_NULL;
  }else if(ConditionType::IN == type_){
    return compare_in_values(row_val);
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
    case CompareOp::IS_NOT_NULL:
      return true;
    default:
      return false;
  }
}

// 用//注释多行是为了调试时方便用多行注释/ *  * /
// LIKE 模式匹配：
//- `%` 或 `*` → 匹配任意字符串（包括空字符串）
//- `%abc%` → 包含 "abc"
//- `abc%` → 以 "abc" 开头
//- `%abc` → 以 "abc" 结尾
//- `abc` → 精确匹配 "abc"
bool ConditionExpr::like_match(const std::string& str,
                               const std::string& pattern) const {
  std::string p = pattern;
  if (p == "%" || p == "*") return true;
  if (p.empty()) return str.empty();

  if (p.front() == '%' && p.back() == '%') {
    std::string mid = p.substr(1, p.length() - 2);
    return str.find(mid) != std::string::npos;
  }
  if (p.back() == '%') {
    std::string prefix = p.substr(0, p.length() - 1);
    return str.find(prefix) == 0;
  }
  if (p.front() == '%') {
    std::string suffix = p.substr(1);
    if (str.length() < suffix.length()) return false;
    return str.substr(str.length() - suffix.length()) == suffix;
  }
  return str == p;
}

// ============================================================
// extract_primary_key_conditions
// ============================================================
ConditionExtractResult ConditionExpr::extract_primary_key_conditions(
    const sql::TableSchema& schema) {
  ConditionExtractResult result;

  // ---- COMPARE 节点 ----
  if (type_ == ConditionType::COMPARE) {
    if (column_ == schema.primary_key_name()) {
      PrimaryKeyFragment frag;
      switch (op_) {
        case CompareOp::EQ:
          frag.is_point_set = true;
          frag.points.push_back(value_);
          break;
        case CompareOp::GT:
          frag.start = value_;
          break;
        case CompareOp::GE:
          frag.start = value_;
          break;
        case CompareOp::LT:
          frag.end = value_;
          break;
        case CompareOp::LE:
          frag.end = value_;
          break;
        default:
          result.remaining = std::make_unique<ConditionExpr>(*this);
          return result;
      }
      if (frag.is_valid()) result.pk_cond.fragments.push_back(frag);
    } else {
      result.remaining = std::make_unique<ConditionExpr>(*this);
    }
    return result;
  }

  // ---- IN 节点 ----
  if (type_ == ConditionType::IN) {
    if (column_ == schema.primary_key_name()) {
      PrimaryKeyFragment frag;
      frag.is_point_set = true;
      frag.points = in_values_;
      if (frag.is_valid()) result.pk_cond.fragments.push_back(frag);
    } else {
      result.remaining = std::make_unique<ConditionExpr>(*this);
    }
    return result;
  }

  // ---- AND 节点 ----
  if (type_ == ConditionType::AND) {
    if (!left_ || !right_) {
      result.remaining = std::make_unique<ConditionExpr>(*this);
      return result;
    }
    auto left_res = left_->extract_primary_key_conditions(schema);
    auto right_res = right_->extract_primary_key_conditions(schema);
    // 合并主键条件（AND 语义）
    auto merged =
        PrimaryKeyCondition::merge_and(left_res.pk_cond, right_res.pk_cond);
    if (merged) {
      result.pk_cond = *merged;
    } else {
      // 无法合并，整个 AND 保留为剩余
      fprintf(stderr, "maybe failed to merge primary key AND conditions for key:%s\n", column_.c_str());
      result.remaining = std::make_unique<ConditionExpr>(*this);
      return result;
    }
    // 合并剩余条件（AND）
    if (left_res.remaining && right_res.remaining) {
      result.remaining = std::make_unique<ConditionExpr>(
          ConditionType::AND, std::move(left_res.remaining),
          std::move(right_res.remaining));
    } else if (left_res.remaining) {
      result.remaining = std::move(left_res.remaining);
    } else if (right_res.remaining) {
      result.remaining = std::move(right_res.remaining);
    }
    return result;
  }

  // ---- OR 节点 ----
  if (type_ == ConditionType::OR) {
    if (!left_ || !right_) {
      result.remaining = std::make_unique<ConditionExpr>(*this);
      return result;
    }
    auto left_res = left_->extract_primary_key_conditions(schema);
    auto right_res = right_->extract_primary_key_conditions(schema);
    // 合并主键条件（OR 语义）
    result.pk_cond =
        PrimaryKeyCondition::merge_or(left_res.pk_cond, right_res.pk_cond);
    // 合并剩余条件（OR）
    if (left_res.remaining && right_res.remaining) {
      result.remaining = std::make_unique<ConditionExpr>(
          ConditionType::OR, std::move(left_res.remaining),
          std::move(right_res.remaining));
    } else if (left_res.remaining) {
      result.remaining = std::move(left_res.remaining);
    } else if (right_res.remaining) {
      result.remaining = std::move(right_res.remaining);
    }
    return result;
  }

  // ---- NOT 节点 ----
  // NOT 通常无法下推，保留为剩余
  result.remaining = std::make_unique<ConditionExpr>(*this);
  return result;
}

// ============================================================
// only_primary_key
// ============================================================
bool ConditionExpr::only_primary_key(const sql::TableSchema& schema) const {
  if (type_ == ConditionType::COMPARE || type_ == ConditionType::IN) {
    return column_ == schema.primary_key_name();
  }
  if (type_ == ConditionType::AND || type_ == ConditionType::OR) {
    return left_ && right_ && left_->only_primary_key(schema) &&
           right_->only_primary_key(schema);
  }
  if (type_ == ConditionType::NOT) {
    return left_ && left_->only_primary_key(schema);
  }
  return false;
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
    case CompareOp::IS_NOT_NULL:
      return "IS NOT NULL";
    case CompareOp::IS_NULL:
      return "IS NULL";
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
  if (str == "IS NOT NULL") return CompareOp::IS_NOT_NULL;
  if (str == "IS NULL") return CompareOp::IS_NULL;
  return CompareOp::EQ;
}

}  // namespace query