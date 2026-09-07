#include "compare_op.h"
#include "relation/value.h"


namespace stmt {


  
bool compare_value(CompareOp op, const sql::Value& row_val, const sql::Value& expr_val) {
  if (row_val.is_null()) {
    return op == CompareOp::IS_NULL;
  }

  switch (op) {
    case CompareOp::EQ:
      return row_val == expr_val;
    case CompareOp::NE:
      return row_val != expr_val;
    case CompareOp::GT:
      return row_val > expr_val;
    case CompareOp::GE:
      return row_val >= expr_val;
    case CompareOp::LT:
      return row_val < expr_val;
    case CompareOp::LE:
      return row_val <= expr_val;
    case CompareOp::LIKE:
      if (!row_val.is_string() || !expr_val.is_string()) return false;
      return like_match(row_val.str_val(), expr_val.str_val());
    case CompareOp::IS_NOT_NULL:
      return true;
    default:
      return false;
  }
}

bool like_match(const std::string& str, const std::string& pattern) {
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

CompareOp flip_compare_op(CompareOp op){
  switch (op) {
    case CompareOp::EQ:
      return CompareOp::NE;
    case CompareOp::NE:
      return CompareOp::EQ;
    case CompareOp::GT:
      return CompareOp::LT;
    case CompareOp::GE:
      return CompareOp::LT;
    case CompareOp::LT:
      return CompareOp::GE;
    case CompareOp::LE:
      return CompareOp::GT;
    case CompareOp::LIKE:
      return CompareOp::UNKNOWN;
    case CompareOp::IS_NOT_NULL:
      return CompareOp::IS_NULL;
    case CompareOp::IS_NULL:
      return CompareOp::IS_NOT_NULL;
    default:
      return CompareOp::UNKNOWN;
  }
}

// ============================================================
// 辅助：判断比较操作符是否可索引
// ============================================================
bool is_indexable_compare_op(CompareOp op) {
  switch (op) {
    case CompareOp::EQ:
    case CompareOp::GE:
    case CompareOp::GT:
    case CompareOp::LE:
    case CompareOp::LT:
    case CompareOp::NE:
      return true;
    case CompareOp::LIKE:
    case CompareOp::IS_NULL:
    case CompareOp::IS_NOT_NULL:
      return false;
    default:
      return false;
  }
}

}


