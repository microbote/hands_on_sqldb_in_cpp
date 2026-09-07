#pragma once


#include <string>

namespace sql {
class Value;
}

namespace stmt {

// ============================================================
// 比较操作符
// ============================================================
enum class CompareOp {
  EQ,           // =
  NE,           // != / <>
  GT,           // >
  GE,           // >=
  LT,           // <
  LE,           // <=
  LIKE,         // LIKE
  IS_NULL,      // IS NULL
  IS_NOT_NULL,  // IS NOT NULL
  UNKNOWN = 101
};

std::string compare_op_to_string(CompareOp op);
CompareOp string_to_compare_op(const std::string& str);

//return unknown if not(op) is not supported
CompareOp flip_compare_op(CompareOp op);

// return true if "row_val op expr_val" matches.
bool compare_value(CompareOp op, const sql::Value& row_val, const sql::Value& expr_val);

bool like_match(const std::string& str, const std::string& pattern);

bool is_indexable_compare_op(CompareOp op);

}// namespace query