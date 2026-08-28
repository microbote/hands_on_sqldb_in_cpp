// types.h
#pragma once

#include <cstdint>
#include <string>
#include <vector>

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
  NULL_TYPE  // NULL 类型
};

// 数据类型名称
inline const char* data_type_name(DataType type) {
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

// 从字符串解析数据类型
inline DataType parse_data_type(const std::string& name) {
  if (name == "INT" || name == "int") return DataType::INT;
  if (name == "BIGINT" || name == "bigint") return DataType::BIGINT;
  if (name == "VARCHAR" || name == "varchar") return DataType::VARCHAR;
  if (name == "TEXT" || name == "text") return DataType::TEXT;
  if (name == "BOOLEAN" || name == "boolean") return DataType::BOOLEAN;
  return DataType::NULL_TYPE;
}

}  // namespace sql