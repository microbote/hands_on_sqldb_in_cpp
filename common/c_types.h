#ifndef COMMON_TYPES_H
#define COMMON_TYPES_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================================
   操作符枚举
   ============================================================ */
typedef enum {
  OP_EQ,
  OP_NE,
  OP_GT,
  OP_GE,
  OP_LT,
  OP_LE,
  OP_AND,
  OP_OR,
  OP_NOT,
  OP_IN,
  OP_NOT_IN,
  OP_ASC,
  OP_DESC,
  OP_IS_NULL,
  OP_IS_NOT_NULL,
  OP_LIKE,
  OP_UNKNOWN
} COpType;

static inline const char *op_to_string(COpType op) {
  switch (op) {
  case OP_EQ:
    return "=";
  case OP_NE:
    return "!=";
  case OP_GT:
    return ">";
  case OP_GE:
    return ">=";
  case OP_LT:
    return "<";
  case OP_LE:
    return "<=";
  case OP_AND:
    return "AND";
  case OP_OR:
    return "OR";
  case OP_NOT:
    return "NOT";
  case OP_IN:
    return "IN";
  case OP_NOT_IN:
    return "NOT IN";
  case OP_ASC:
    return "ASC";
  case OP_DESC:
    return "DESC";
  case OP_IS_NULL:
    return "IS NULL";
  case OP_IS_NOT_NULL:
    return "IS NOT NULL";
  case OP_LIKE:
    return "LIKE";
  default:
    return "UNKNOWN";
  }
}

/* ============================================================
   数据类型枚举（DDL 使用）
   ============================================================ */
typedef enum {
  DT_INT,
  DT_BIGINT,
  DT_VARCHAR,
  DT_TEXT,
  DT_BOOLEAN,
  DT_NULL,
  DT_UNKNOWN,
  /* 以下为新增类型（追加在末尾，保持既有枚举值不变） */
  DT_TINYINT,
  DT_SMALLINT,
  DT_DATE,
  DT_TIME,
  DT_DATETIME,
  DT_CHAR
} CDataType;

static inline const char *data_type_to_string(CDataType dt) {
  switch (dt) {
  case DT_INT:
    return "INT";
  case DT_BIGINT:
    return "BIG_INT";
  case DT_VARCHAR:
    return "VARCHAR";
  case DT_TEXT:
    return "TEXT";
  case DT_BOOLEAN:
    return "BOOLEAN";
  case DT_NULL:
    return "NULL";
  case DT_TINYINT:
    return "TINYINT";
  case DT_SMALLINT:
    return "SMALLINT";
  case DT_DATE:
    return "DATE";
  case DT_TIME:
    return "TIME";
  case DT_DATETIME:
    return "DATETIME";
  case DT_CHAR:
    return "CHAR";
  default:
    return "UNKNOWN";
  }
}

/* ============================================================
   类型别名表（唯一数据源）

   这一份定义同时被两侧使用：
     - C 侧：parser 的 lexer/grammar（只 include 本头文件，不依赖 C++）
     - C++ 侧：sql_types 的 string_to_data_type() / kMax*Length
   新增/修改类型别名只需要改这里的 c_type_aliases[]。
   ============================================================ */

/* 字符串类型的长度上限（显式声明 (n) 时的合法范围） */
#define CTYPE_MAX_CHAR_LEN 255
#define CTYPE_MAX_VARCHAR_LEN 65535
#define CTYPE_MAX_TEXT_LEN 65535

typedef struct CTypeAlias {
  const char *name;   /* 小写类型名（比较时大小写不敏感） */
  CDataType type;
} CTypeAlias;

static const CTypeAlias c_type_aliases[] = {
    {"tinyint", DT_TINYINT},
    {"int8", DT_TINYINT},
    {"smallint", DT_SMALLINT},
    {"int16", DT_SMALLINT},
    {"int", DT_INT},
    {"integer", DT_INT},
    {"int32", DT_INT},
    {"bigint", DT_BIGINT},
    {"int64", DT_BIGINT},
    {"char", DT_CHAR},
    {"character", DT_CHAR},
    {"varchar", DT_VARCHAR},
    {"varying", DT_VARCHAR},
    {"text", DT_TEXT},
    {"boolean", DT_BOOLEAN},
    {"bool", DT_BOOLEAN},
    {"date", DT_DATE},
    {"time", DT_TIME},
    {"datetime", DT_DATETIME},
    {"timestamp", DT_DATETIME},
    {"null", DT_NULL},
};

#define CTYPE_ALIAS_COUNT \
  ((int)(sizeof(c_type_aliases) / sizeof(c_type_aliases[0])))

static inline char c_type_lower(char c) {
  return (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c;
}

/* 大小写不敏感的字符串相等 */
static inline int c_type_streq_ci(const char *a, const char *b) {
  if (a == NULL || b == NULL) {
    return a == b;
  }
  while (*a != '\0' && *b != '\0') {
    if (c_type_lower(*a) != c_type_lower(*b)) {
      return 0;
    }
    ++a;
    ++b;
  }
  return *a == *b;
}

/* 类型名 -> CDataType（未知返回 DT_UNKNOWN） */
static inline CDataType c_type_lookup(const char *name) {
  int i;
  if (name == NULL) {
    return DT_UNKNOWN;
  }
  for (i = 0; i < CTYPE_ALIAS_COUNT; ++i) {
    if (c_type_streq_ci(name, c_type_aliases[i].name)) {
      return c_type_aliases[i].type;
    }
  }
  return DT_UNKNOWN;
}

static inline int c_type_is_string(CDataType type) {
  return type == DT_CHAR || type == DT_VARCHAR || type == DT_TEXT;
}

/* 只有 CHAR/VARCHAR 允许显式写 (n) */
static inline int c_type_accepts_length(CDataType type) {
  return type == DT_CHAR || type == DT_VARCHAR;
}

/* 显式长度的上限；不接受长度的类型返回 0 */
static inline unsigned c_type_max_length(CDataType type) {
  switch (type) {
  case DT_CHAR:
    return CTYPE_MAX_CHAR_LEN;
  case DT_VARCHAR:
    return CTYPE_MAX_VARCHAR_LEN;
  default:
    return 0;
  }
}

/* 显式声明 (n) 的合法性：n >= 1 且不超过该类型上限 */
static inline int c_type_length_valid(CDataType type, unsigned length) {
  if (length == 0) {
    return 0;
  }
  return c_type_accepts_length(type) && length <= c_type_max_length(type);
}

#ifdef __cplusplus
}
#endif

#endif
