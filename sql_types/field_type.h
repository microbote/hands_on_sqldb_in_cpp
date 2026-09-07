// types.h
#pragma once

#include "common/c_types.h"
#include <cstdint>

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
  NULL_TYPE,  // NULL 类型
  UNKNOWN_TYPE
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

inline DataType from_c(::CDataType type){
  //{ DT_INT, DT_BIGINT, DT_VARCHAR, DT_TEXT, DT_BOOLEAN } 
  switch(type){
    case DT_INT: return DataType::INT;
    case DT_BIGINT: return DataType::BIGINT;
    case DT_VARCHAR: return DataType::VARCHAR;
    case DT_TEXT: return DataType::TEXT;
    case DT_BOOLEAN: return DataType::BOOLEAN;
    case DT_NULL: return DataType::NULL_TYPE;
    default: return DataType::UNKNOWN_TYPE;
  }
}

inline CDataType to_c(DataType type){
  switch(type){
    case DataType::INT: return DT_INT;
    case DataType::BIGINT: return DT_BIGINT;
    case DataType::VARCHAR: return DT_VARCHAR;
    case DataType::TEXT: return DT_TEXT;
    case DataType::BOOLEAN: return DT_BOOLEAN;
    case DataType::NULL_TYPE: return DT_NULL;
    default: return DT_UNKNOWN;
  }
}



}  // namespace sql