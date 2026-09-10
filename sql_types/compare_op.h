// compare_op.h
#pragma once

#include <cstdint>
#include <string>

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

}  // namespace sql
