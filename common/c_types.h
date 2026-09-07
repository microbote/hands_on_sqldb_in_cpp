#ifndef COMMON_TYPES_H
#define COMMON_TYPES_H

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
  DT_UNKNOWN
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
  default:
    return "UNKNOWN";
  }
}

#ifdef __cplusplus
}
#endif

#endif