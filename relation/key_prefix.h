// key_prefix.h
#pragma once

#include <string>
#include "value.h"

namespace sql {

namespace keys {
// ============================================================
// 系统元数据 Key
// ============================================================

// 所有数据库列表
inline std::string databases() { return "@system/databases"; }

// 某个数据库的表列表
inline std::string db_tables(const std::string& db_name) {
  return "@system/db/" + db_name + "/tables";
}

// 某个表的 schema
inline std::string db_schema(const std::string& db_name,
                             const std::string& table_name) {
  return "@system/db/" + db_name + "/schema/" + table_name;
}

// ============================================================
// 数据 Key
// ============================================================

// 数据前缀
inline std::string data_prefix(const std::string& db_name,
                               const std::string& table_name) {
  return "@data/" + db_name + "/" + table_name + "/";
}

// 数据 Key
inline std::string data_key(const std::string& db_name,
                            const std::string& table_name,
                            const std::string& primary_key) {
  return data_prefix(db_name, table_name) + primary_key;
}

// 数据 Key（从 Value 构造）
inline std::string data_key(const std::string& db_name,
                            const std::string& table_name,
                            const Value& primary_key) {
  return data_key(db_name, table_name, primary_key.to_string());
}

inline std::string data_prefix_key(const std::string& prefix,
                                   const std::string& primary_key) {
  return prefix + primary_key;
}

inline std::string data_prefix_key(const std::string& prefix,
                                   const Value& primary_key) {
  return data_prefix_key(prefix, primary_key.to_string());
}

// ============================================================
// Key 类型检查
// ============================================================

inline bool is_system_key(const std::string& key) {
  return key.size() >= 8 && key.substr(0, 8) == "@system/";
}

inline bool is_data_key(const std::string& key) {
  return key.size() >= 6 && key.substr(0, 6) == "@data/";
}

}  // namespace keys
}  // namespace sql