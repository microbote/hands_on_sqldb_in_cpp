#pragma once

#include "common/c_types.h"

#include <cctype>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace sql {

// ============================================================
// SQL 数据类型枚举
// ============================================================
enum class DataType : uint8_t {
  INT,       // 64位整数
  BIGINT,    // 64位整数 (别名)
  VARCHAR,   // 变长字符串
  TEXT,      // 长文本
  BOOLEAN,   // 布尔值
  NULL_TYPE, // NULL 类型
  UNKNOWN_TYPE
};

enum class DataTypeClass : uint8_t {
  INTEGER,
  STRING,
  BOOLEAN,
  UNKNOWN_CLASS
};

inline bool is_integer(DataType type) {
  switch (type) {
  case DataType::INT:
  case DataType::BIGINT:
    return true;
  default:
    return false;
  }
}

inline bool is_string(DataType type) {
  switch (type) {
  case DataType::VARCHAR:
  case DataType::TEXT:
    return true;
  default:
    return false;
  }
}

inline bool is_boolean(DataType type) { return type == DataType::BOOLEAN; }

inline bool is_numeric(DataType type) { return is_integer(type); }

inline bool is_null(DataType type) {
  return type == DataType::NULL_TYPE || type == DataType::UNKNOWN_TYPE;
}

// ============================================================
// 类型族（type family）
//
// INT/BIGINT 在底层都按 int64_t 存储与编码，VARCHAR/TEXT 在底层都是
// 字符串。因此"列类型 vs 值类型"的一致性判定必须按 *族* 而不是按
// 枚举值精确相等，否则 Value(int64_t(5)) 永远写不进 BIGINT 列、
// TEXT 往返一次就会被判定为 VARCHAR。
//
// 注意：这里只表达"可以直接赋值/比较"，不做精度收窄（BIGINT -> INT
// 依然算兼容，由上层决定是否检查取值范围）。
// ============================================================
inline bool is_same_family(DataType a, DataType b) {
  if (a == b) {
    return true;
  }
  if (is_integer(a) && is_integer(b)) {
    return true;
  }
  if (is_string(a) && is_string(b)) {
    return true;
  }
  return false;
}

// 大小写不敏感的 ASCII 比较（不分配内存）
inline bool iequals_ascii(std::string_view a, std::string_view b) {
  if (a.size() != b.size()) {
    return false;
  }
  for (size_t i = 0; i < a.size(); ++i) {
    if (std::tolower(static_cast<unsigned char>(a[i])) !=
        std::tolower(static_cast<unsigned char>(b[i]))) {
      return false;
    }
  }
  return true;
}

inline bool is_orderable(DataType type) {
  return is_integer(type) || is_string(type) || is_boolean(type);
}

inline bool is_hashable(DataType type) {
  return is_integer(type) || is_string(type);
}

inline bool is_indexable(DataType type) { return is_orderable(type); }

inline DataTypeClass get_type_class(DataType type){
  if(is_integer(type)){
    return DataTypeClass::INTEGER;
  }
  if(is_string(type)){
    return DataTypeClass::STRING;
  }
  if(is_boolean(type)){
    return DataTypeClass::BOOLEAN;
  }
  return DataTypeClass::UNKNOWN_CLASS;
}

// ============================================================
// 数据类型名称
// ============================================================
inline const char *data_type_name(DataType type) {
  switch (type) {
  case DataType::INT:
    return "INT";
  case DataType::BIGINT:
    return "BIGINT";
  case DataType::VARCHAR:
    return "VARCHAR";
  case DataType::TEXT:
    return "TEXT";
  case DataType::BOOLEAN:
    return "BOOLEAN";
  case DataType::NULL_TYPE:
    return "NULL";
  default:
    return "UNKNOWN";
  }
}

// ============================================================
// string_to_data_type：字符串转换为 DataType
// 支持大小写不敏感，以及常见别名：
//   INT / INTEGER      -> INT
//   BIGINT             -> BIGINT
//   VARCHAR / VARYING  -> VARCHAR
//   TEXT               -> TEXT
//   BOOLEAN / BOOL     -> BOOLEAN
//   NULL               -> NULL_TYPE
// 未知类型返回 DataType::UNKNOWN_TYPE
// ============================================================
// ============================================================
// 字符串转类型（唯一版本，接受 std::string_view）
// string_to_data_type("INT")          -> DataType::INT
// string_to_data_type(std::string(...)) -> DataType::INT
// ============================================================
inline DataType string_to_data_type(std::string_view str) {
  // 大小写无关比较（避免拷贝）
  auto iequals = [](std::string_view s, std::string_view literal) {
    return iequals_ascii(s, literal);
  };

  if (iequals(str, "int") || iequals(str, "integer")|| iequals(str, "int8")) {
    return DataType::INT;
  }
  if (iequals(str, "bigint") ) {
    return DataType::BIGINT;
  }
  if (iequals(str, "varchar") ) {
    return DataType::VARCHAR;
  }
  
  if (iequals(str, "text")) {
    return DataType::TEXT;
  }
  if (iequals(str, "boolean") || iequals(str, "bool")) {
    return DataType::BOOLEAN;
  }
  if (iequals(str, "null")) {
    return DataType::NULL_TYPE;
  }
  return DataType::UNKNOWN_TYPE;
}

// ============================================================
// C-API 互操作
// ============================================================
inline CDataType to_c(DataType type) {
  switch (type) {
  case DataType::INT:
    return DT_INT;
  case DataType::BIGINT:
    return DT_BIGINT;
  case DataType::VARCHAR:
    return DT_VARCHAR;
  case DataType::TEXT:
    return DT_TEXT;
  case DataType::BOOLEAN:
    return DT_BOOLEAN;
  case DataType::NULL_TYPE:
    return DT_NULL;
  default:
    return DT_UNKNOWN;
  }
}

inline DataType from_c(::CDataType type) {
  switch (type) {
  case DT_INT:
    return DataType::INT;
  case DT_BIGINT:
    return DataType::BIGINT;
  case DT_VARCHAR:
    return DataType::VARCHAR;
  case DT_TEXT:
    return DataType::TEXT;
  case DT_BOOLEAN:
    return DataType::BOOLEAN;
  case DT_NULL:
    return DataType::NULL_TYPE;
  default:
    return DataType::UNKNOWN_TYPE;
  }
}

} // namespace sql
