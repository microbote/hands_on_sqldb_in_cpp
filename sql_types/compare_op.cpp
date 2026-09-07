// compare_op.cpp
#include "compare_op.h"

#include <algorithm>
#include <cctype>

namespace sql {

std::string compare_op_to_string(CompareOp op) {
  switch (op) {
    case CompareOp::EQ:           return "=";
    case CompareOp::NE:           return "!=";
    case CompareOp::GT:           return ">";
    case CompareOp::GE:           return ">=";
    case CompareOp::LT:           return "<";
    case CompareOp::LE:           return "<=";
    case CompareOp::LIKE:         return "LIKE";
    case CompareOp::NOT_LIKE:     return "NOT LIKE";
    case CompareOp::IS_NULL:      return "IS NULL";
    case CompareOp::IS_NOT_NULL:  return "IS NOT NULL";
    default:                      return "UNKNOWN";
  }
}

CompareOp string_to_compare_op(const std::string& str) {
  if (str == "=") return CompareOp::EQ;
  if (str == "!=" || str == "<>") return CompareOp::NE;
  if (str == ">") return CompareOp::GT;
  if (str == ">=") return CompareOp::GE;
  if (str == "<") return CompareOp::LT;
  if (str == "<=") return CompareOp::LE;

  // 大小写不敏感
  std::string upper;
  upper.reserve(str.size());
  for (char c : str) {
    upper.push_back(static_cast<char>(std::toupper(static_cast<unsigned char>(c))));
  }

  if (upper == "LIKE") return CompareOp::LIKE;
  if (upper == "NOT LIKE") return CompareOp::NOT_LIKE;
  if (upper == "IS NULL") return CompareOp::IS_NULL;
  if (upper == "IS NOT NULL") return CompareOp::IS_NOT_NULL;

  return CompareOp::UNKNOWN;
}

CompareOp flip_compare_op(CompareOp op) {
  switch (op) {
    case CompareOp::EQ:           return CompareOp::NE;
    case CompareOp::NE:           return CompareOp::EQ;
    case CompareOp::GT:           return CompareOp::LE;
    case CompareOp::GE:           return CompareOp::LT;
    case CompareOp::LT:           return CompareOp::GE;
    case CompareOp::LE:           return CompareOp::GT;
    case CompareOp::LIKE:         return CompareOp::NOT_LIKE;
    case CompareOp::NOT_LIKE:     return CompareOp::LIKE;
    case CompareOp::IS_NULL:      return CompareOp::IS_NOT_NULL;
    case CompareOp::IS_NOT_NULL:  return CompareOp::IS_NULL;
    default:                      return CompareOp::UNKNOWN;
  }
}

}  // namespace sql