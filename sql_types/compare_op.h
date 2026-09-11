// compare_op.h
#pragma once

#include <cstdint>
#include <string>

#include "common/c_types.h"

namespace sql {

// ============================================================
// 比较操作符枚举（仅用于单值比较）
// IN/NOT_IN 不是比较操作符，是单独的节点类型
// ============================================================
enum class CompareOp : uint8_t {
  EQ,           // =
  NE,           // != / <>
  GT,           // >
  GE,           // >=
  LT,           // <
  LE,           // <=
  LIKE,         // LIKE pattern
  NOT_LIKE,     // NOT LIKE pattern
  IS_NULL,      // IS NULL
  IS_NOT_NULL,  // IS NOT NULL
  UNKNOWN
};

// ============================================================
// 特性判断（inline，放头文件）
// ============================================================

// 是范围比较（可用于区间扫描：age > 5, id <= 10）
inline bool is_range_op(CompareOp op) {
  return op == CompareOp::GT || op == CompareOp::GE ||
         op == CompareOp::LT || op == CompareOp::LE;
}

// 是等值比较
inline bool is_equality_op(CompareOp op) {
  return op == CompareOp::EQ || op == CompareOp::NE;
}

// 是 NULL 判断
inline bool is_null_op(CompareOp op) {
  return op == CompareOp::IS_NULL || op == CompareOp::IS_NOT_NULL;
}

// 是 LIKE 模式匹配
inline bool is_like_op(CompareOp op) {
  return op == CompareOp::LIKE || op == CompareOp::NOT_LIKE;
}

// 是否可能需要全表扫描（不能用于索引快速定位）
inline bool require_full_scan(CompareOp op) {
  return op == CompareOp::LIKE || op == CompareOp::NOT_LIKE ||
         op == CompareOp::IS_NULL || op == CompareOp::IS_NOT_NULL;
}

// 是否可用于索引搜索（可生成 KeyRange / KeySet）
inline bool is_indexable(CompareOp op) {
  switch (op) {
    case CompareOp::EQ:
    case CompareOp::GT:
    case CompareOp::GE:
    case CompareOp::LT:
    case CompareOp::LE:
      return true;
    default:
      return false;
  }
}

// ============================================================
// 字符串互转（声明，实现放 .cpp）
// ============================================================
std::string compare_op_to_string(CompareOp op);
CompareOp string_to_compare_op(const std::string& str);

// 翻转（NOT 下推用）：EQ ↔ NE, GT ↔ LE, GE ↔ LT
CompareOp flip_compare_op(CompareOp op);

// ============================================================
// C-API 互操作（与 DataType ↔ CDataType 对应）
// AST/parser 用的是 COpType，进入 sql_types 之前必须转换到这里，
// 不要在 statement 层再写第三套字符串映射。
// ============================================================
inline COpType to_c(CompareOp op) {
  switch (op) {
  case CompareOp::EQ:
    return OP_EQ;
  case CompareOp::NE:
    return OP_NE;
  case CompareOp::GT:
    return OP_GT;
  case CompareOp::GE:
    return OP_GE;
  case CompareOp::LT:
    return OP_LT;
  case CompareOp::LE:
    return OP_LE;
  case CompareOp::LIKE:
    return OP_LIKE;
  case CompareOp::NOT_LIKE:
    return OP_NOT_LIKE;
  case CompareOp::IS_NULL:
    return OP_IS_NULL;
  case CompareOp::IS_NOT_NULL:
    return OP_IS_NOT_NULL;
  default:
    return OP_UNKNOWN;
  }
}

inline CompareOp from_c(::COpType op) {
  switch (op) {
  case OP_EQ:
    return CompareOp::EQ;
  case OP_NE:
    return CompareOp::NE;
  case OP_GT:
    return CompareOp::GT;
  case OP_GE:
    return CompareOp::GE;
  case OP_LT:
    return CompareOp::LT;
  case OP_LE:
    return CompareOp::LE;
  case OP_LIKE:
    return CompareOp::LIKE;
  case OP_NOT_LIKE:
    return CompareOp::NOT_LIKE;
  case OP_IS_NULL:
    return CompareOp::IS_NULL;
  case OP_IS_NOT_NULL:
    return CompareOp::IS_NOT_NULL;
  default:
    return CompareOp::UNKNOWN;
  }
}

}  // namespace sql
