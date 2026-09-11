// sql_truth.h
#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <string_view>
#include <utility>

#include "compare_op.h"
#include "condition.h"
#include "condition_types.h"
#include "field_type.h"
#include "temporal.h"
#include "value.h"

namespace sql {

// ============================================================
// SQL 三值逻辑
//
//  真值集合 = {TRUE, FALSE, UNKNOWN}
//
//  NULL 不是普通值，它表示 UNKNOWN。任何与 NULL 的比较结果都是
//  UNKNOWN：
//      NULL = 5        -> UNKNOWN
//      NULL < 5        -> UNKNOWN
//      NULL = NULL     -> UNKNOWN   （注意：不是 TRUE）
//      NULL <> NULL    -> UNKNOWN
//  而 IS NULL / IS NOT NULL 是专门"探测 NULL"的谓词，结果是二值的：
//      NULL IS NULL     -> TRUE
//      NULL IS NOT NULL -> FALSE
//      5 IS NULL        -> FALSE
//  真值表（SQL 标准 / MySQL 一致）：
//      FALSE AND UNKNOWN = FALSE       TRUE AND UNKNOWN = UNKNOWN
//      TRUE  OR  UNKNOWN = TRUE        FALSE OR UNKNOWN = UNKNOWN
//      NOT UNKNOWN       = UNKNOWN
//  WHERE 只保留 TRUE：FALSE 与 UNKNOWN 都会被过滤。
//
// 注意：Value 的 operator== 是 C++ 容器语义（NULL == NULL 为 true），
// 存储层的全序在 KeyCodecs。这里才是 SQL 的比较语义。
// ============================================================

enum class Truth : uint8_t {
  FALSE = 0,
  TRUE = 1,
  UNKNOWN = 2,
};

inline const char* truth_to_string(Truth t) {
  switch (t) {
  case Truth::TRUE:
    return "TRUE";
  case Truth::FALSE:
    return "FALSE";
  default:
    return "UNKNOWN";
  }
}

inline Truth truth_from_bool(bool b) {
  return b ? Truth::TRUE : Truth::FALSE;
}

inline Truth truth_not(Truth t) {
  switch (t) {
  case Truth::TRUE:
    return Truth::FALSE;
  case Truth::FALSE:
    return Truth::TRUE;
  default:
    return Truth::UNKNOWN;
  }
}

inline Truth truth_and(Truth a, Truth b) {
  if (a == Truth::FALSE || b == Truth::FALSE) {
    return Truth::FALSE;  // FALSE 一票否决
  }
  if (a == Truth::UNKNOWN || b == Truth::UNKNOWN) {
    return Truth::UNKNOWN;
  }
  return Truth::TRUE;
}

inline Truth truth_or(Truth a, Truth b) {
  if (a == Truth::TRUE || b == Truth::TRUE) {
    return Truth::TRUE;   // TRUE 一票通过
  }
  if (a == Truth::UNKNOWN || b == Truth::UNKNOWN) {
    return Truth::UNKNOWN;
  }
  return Truth::FALSE;
}

// WHERE 子句：只有 TRUE 保留
inline bool where_keeps(Truth t) { return t == Truth::TRUE; }

// ------------------------------------------------------------
// LIKE 匹配：% 任意长度，_ 单字符，\ 转义
// 目前是二进制比较（大小写敏感）；collation 留待后续处理。
// ------------------------------------------------------------
inline bool like_match(std::string_view text, std::string_view pattern) {
  size_t ti = 0;
  size_t pi = 0;
  while (pi < pattern.size()) {
    char pc = pattern[pi];
    if (pc == '\\' && pi + 1 < pattern.size()) {
      pc = pattern[pi + 1];
      if (ti >= text.size() || text[ti] != pc) {
        return false;
      }
      ++ti;
      pi += 2;
      continue;
    }
    if (pc == '%') {
      ++pi;
      while (pi < pattern.size() && pattern[pi] == '%') {
        ++pi;  // 折叠连续的 %
      }
      if (pi == pattern.size()) {
        return true;
      }
      for (size_t k = ti; k <= text.size(); ++k) {
        if (like_match(text.substr(k), pattern.substr(pi))) {
          return true;
        }
      }
      return false;
    }
    if (pc == '_') {
      if (ti >= text.size()) {
        return false;
      }
      ++ti;
      ++pi;
      continue;
    }
    if (ti >= text.size() || text[ti] != pc) {
      return false;
    }
    ++ti;
    ++pi;
  }
  return ti == text.size();
}

namespace detail {

inline std::optional<int64_t> int64_of(const Value& v) {
  if (v.is_bool()) {
    return v.as_bool() ? 1 : 0;
  }
  if (v.is_int()) {
    return v.as_int();
  }
  return std::nullopt;
}

inline std::optional<int64_t> temporal_seconds(const Value& v) {
  if (v.is_date()) {
    return v.as_int() * kSecondsPerDay;
  }
  if (v.is_time() || v.is_datetime()) {
    return v.as_int();
  }
  return std::nullopt;
}

inline std::optional<int64_t> parse_temporal(DataType type,
                                             std::string_view text) {
  if (type == DataType::DATE) {
    return temporal::parse_date(text);
  }
  if (type == DataType::TIME) {
    return temporal::parse_time(text);
  }
  if (type == DataType::DATETIME) {
    return temporal::parse_datetime(text);
  }
  return std::nullopt;
}

inline int cmp_int64(int64_t a, int64_t b) {
  if (a == b) {
    return 0;
  }
  return a < b ? -1 : 1;
}

}  // namespace detail

// ============================================================
// 顺序比较：返回 -1/0/1；UNKNOWN（NULL / 不可比）返回 nullopt
//
// 类型提升：
//   整型 & 布尔  -> int64 比较（TINYINT/SMALLINT/INT/BIGINT 互通）
//   字符串       -> 字节序比较
//   时间         -> 统一到秒（DATE 的天数 * 86400）
//   数值 vs 字符串 -> 字符串按整数解析（失败 -> UNKNOWN）
//   时间 vs 字符串 -> 字符串按该时间类型解析（失败 -> UNKNOWN）
//   其它跨族组合   -> UNKNOWN
// ============================================================
inline std::optional<int> sql_order(const Value& a, const Value& b) {
  if (a.is_null() || b.is_null()) {
    return std::nullopt;   // 与 NULL 比较 -> UNKNOWN
  }

  const DataType common = common_type(a.type(), b.type());

  // 1) 整型 / 布尔
  if (is_integer(common) || is_boolean(common)) {
    const auto lhs = detail::int64_of(a);
    const auto rhs = detail::int64_of(b);
    if (!lhs.has_value() || !rhs.has_value()) {
      return std::nullopt;
    }
    return detail::cmp_int64(*lhs, *rhs);
  }

  // 2) 字符串
  if (is_string(common)) {
    const int cmp = a.as_str().compare(b.as_str());
    return cmp == 0 ? 0 : (cmp < 0 ? -1 : 1);
  }

  // 3) 时间
  if (is_temporal(common)) {
    const auto lhs = detail::temporal_seconds(a);
    const auto rhs = detail::temporal_seconds(b);
    if (!lhs.has_value() || !rhs.has_value()) {
      return std::nullopt;
    }
    return detail::cmp_int64(*lhs, *rhs);
  }

  // 4) 跨族：一边字符串，另一边数值或时间
  if (a.is_string() != b.is_string()) {
    const bool flipped = a.is_string();
    const Value& text = flipped ? a : b;
    const Value& other = flipped ? b : a;

    std::optional<int> result;
    if (is_numeric(other.type())) {
      int64_t parsed = 0;
      if (!parse_int64_strict(text.as_str(), parsed)) {
        return std::nullopt;   // 非数字字符串：判 UNKNOWN（比 MySQL 更保守）
      }
      const auto other_value = detail::int64_of(other);
      if (!other_value.has_value()) {
        return std::nullopt;
      }
      result = detail::cmp_int64(*other_value, parsed);
    } else if (is_temporal(other.type())) {
      const auto parsed = detail::parse_temporal(other.type(), text.as_str());
      const auto other_value = detail::temporal_seconds(other);
      if (!parsed.has_value() || !other_value.has_value()) {
        return std::nullopt;
      }
      // 解析结果与被比较值的单位一致（DATE -> 天，DATETIME/TIME -> 秒）
      const int64_t scaled =
          other.is_date() ? *parsed * kSecondsPerDay : *parsed;
      result = detail::cmp_int64(*other_value, scaled);
    } else {
      return std::nullopt;
    }

    return flipped ? -*result : *result;
  }

  return std::nullopt;   // 其它跨族组合不可比
}

// 单个比较谓词（含 LIKE / IS [NOT] NULL）
inline Truth sql_compare_op(CompareOp op, const Value& a, const Value& b) {
  switch (op) {
  case CompareOp::IS_NULL:
    return truth_from_bool(a.is_null());
  case CompareOp::IS_NOT_NULL:
    return truth_from_bool(!a.is_null());
  case CompareOp::LIKE:
  case CompareOp::NOT_LIKE: {
    if (a.is_null() || b.is_null() || !a.is_string() || !b.is_string()) {
      return Truth::UNKNOWN;
    }
    const bool matched = like_match(a.as_str(), b.as_str());
    return truth_from_bool(op == CompareOp::LIKE ? matched : !matched);
  }
  default:
    break;
  }

  const auto order = sql_order(a, b);
  if (!order.has_value()) {
    return Truth::UNKNOWN;
  }
  switch (op) {
  case CompareOp::EQ:
    return truth_from_bool(*order == 0);
  case CompareOp::NE:
    return truth_from_bool(*order != 0);
  case CompareOp::GT:
    return truth_from_bool(*order > 0);
  case CompareOp::GE:
    return truth_from_bool(*order >= 0);
  case CompareOp::LT:
    return truth_from_bool(*order < 0);
  case CompareOp::LE:
    return truth_from_bool(*order <= 0);
  default:
    return Truth::UNKNOWN;
  }
}

// SQL 等值/不等/小于（NULL 参与 -> UNKNOWN）
inline Truth sql_equal(const Value& a, const Value& b) {
  return sql_compare_op(CompareOp::EQ, a, b);
}

inline Truth sql_not_equal(const Value& a, const Value& b) {
  return sql_compare_op(CompareOp::NE, a, b);
}

inline Truth sql_less(const Value& a, const Value& b) {
  return sql_compare_op(CompareOp::LT, a, b);
}

// NULL 安全的等值：NULL 与 NULL 视为同一组（DISTINCT / GROUP BY / JOIN 键）
inline Truth sql_equal_null_safe(const Value& a, const Value& b) {
  if (a.is_null() || b.is_null()) {
    return truth_from_bool(a.is_null() && b.is_null());
  }
  return sql_equal(a, b);
}

// ============================================================
// 条件树求值（WHERE）
//
// lookup 返回列值；返回 nullptr 表示列不存在 -> UNKNOWN。
// 只有 where_keeps(truth) == true（即 TRUE）的行才会被保留。
// ============================================================
using ValueLookup = std::function<const Value*(const Identifier&)>;

class ConditionEvaluator : public ConditionVisitorBase {
 public:
  explicit ConditionEvaluator(ValueLookup lookup)
      : lookup_(std::move(lookup)) {}

  Truth result() const { return result_; }

  void visit(const CompareCondition& cond) override {
    const Value* lhs = lookup_(cond.column());
    if (lhs == nullptr) {
      result_ = Truth::UNKNOWN;   // 未知列
      return;
    }
    result_ = sql_compare_op(cond.op(), *lhs, cond.value());
  }

  void visit(const InCondition& cond) override {
    const Value* lhs = lookup_(cond.column());
    if (lhs == nullptr) {
      result_ = Truth::UNKNOWN;
      return;
    }
    // x IN (a, b) ≡ x = a OR x = b，逐项用三值逻辑合并
    Truth acc = Truth::FALSE;
    for (const auto& candidate : cond.values()) {
      acc = truth_or(acc, sql_compare_op(CompareOp::EQ, *lhs, candidate));
    }
    if (cond.is_not_in()) {
      acc = truth_not(acc);
    }
    result_ = acc;
  }

  void visit(const AndCondition& cond) override {
    Truth acc = Truth::TRUE;
    for (size_t i = 0; i < cond.child_count(); ++i) {
      acc = truth_and(acc, evaluate(*cond.child_at(i)));
      if (acc == Truth::FALSE) {
        break;   // FALSE 一票否决，可以提前结束
      }
    }
    result_ = acc;
  }

  void visit(const OrCondition& cond) override {
    Truth acc = Truth::FALSE;
    for (size_t i = 0; i < cond.child_count(); ++i) {
      acc = truth_or(acc, evaluate(*cond.child_at(i)));
      if (acc == Truth::TRUE) {
        break;   // TRUE 一票通过
      }
    }
    result_ = acc;
  }

  void visit(const NotCondition& cond) override {
    result_ = truth_not(evaluate(*cond.child()));
  }

  Truth evaluate(const Condition& cond) {
    cond.accept(*this);
    return result_;
  }

 private:
  ValueLookup lookup_;
  Truth result_ = Truth::UNKNOWN;
};

inline Truth evaluate_condition(const Condition& cond, ValueLookup lookup) {
  ConditionEvaluator evaluator(std::move(lookup));
  return evaluator.evaluate(cond);
}

}  // namespace sql
