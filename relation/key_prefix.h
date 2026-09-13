// key_prefix.h
//
// KV key 的布局（存储契约）：
//
//   系统元数据：@system/databases
//               @system/tables/<db>
//               @system/schema/<db>/<table>
//   表数据：    @data/<db>/<table>/ + KeyCodecs::to_key(pk, 主键列类型)
//
// 两个关键点：
//
// 1) **主键必须用 KeyCodecs 编码**，不能用 Value::to_string()。
//    编码是保序的（同族内值序 == 字节序），扫描顺序、区间边界、
//    "ORDER BY 主键 + LIMIT 早停" 全靠它；用 to_string() 会得到
//    '10' < '9' 这种错误顺序。NULL 也由编码负责（NULL 最小）。
//
// 2) 库名/表名用**长度前缀**编码（"5:users"），不用裸分隔符：
//    既避免名字里出现分隔符时串键，也让"前缀上界"（prefix_end）
//    变得精确 —— 扫描整张表就是 [prefix, prefix_end(prefix))。
//
// 名字统一用 Identifier 的**小写形式**做 key：引擎里标识符大小写不敏感，
// 若用原始大小写，CREATE DATABASE MyDB 之后 USE mydb 会查不到。
#pragma once

#include <cstddef>
#include <string>
#include <string_view>

#include "sql_types/identifier.h"
#include "sql_types/key.h"
#include "sql_types/value.h"

namespace sql::keys {

// ============================================================
// 名字编码
// ============================================================

// "users" -> "5:users"（长度前缀，自定界）
inline std::string encode_component(std::string_view name) {
  return std::to_string(name.size()) + ":" + std::string(name);
}

// 标识符版本：用小写形式（标识符大小写不敏感）
inline std::string encode_identifier(const Identifier &name) {
  return encode_component(name.lower_view());
}

// 前缀的排他上界：把最后一个字节 +1，任何以 prefix 开头的 key 都 < 它。
// 例："@data/1:a/5:users/" -> "@data/1:a/5:users0"
// 返回空串表示没有上界（prefix 为空或全是 0xFF）。
inline std::string prefix_end(std::string prefix) {
  while (!prefix.empty() && static_cast<unsigned char>(prefix.back()) == 0xFF) {
    prefix.pop_back();
  }
  if (prefix.empty()) {
    return std::string();
  }
  prefix.back() =
      static_cast<char>(static_cast<unsigned char>(prefix.back()) + 1);
  return prefix;
}

// ============================================================
// 系统元数据 Key
// ============================================================

// 所有数据库列表
inline std::string databases() { return "@system/databases"; }

// 某个数据库的表列表
inline std::string db_tables(const Identifier &db) {
  return "@system/tables/" + encode_identifier(db);
}

// 某个表的 schema
inline std::string db_schema(const Identifier &db, const Identifier &table) {
  return "@system/schema/" + encode_identifier(db) + "/" +
         encode_identifier(table);
}

// 某个库下所有 schema 的前缀（DROP DATABASE 用）
inline std::string db_schema_prefix(const Identifier &db) {
  return "@system/schema/" + encode_identifier(db) + "/";
}

// ============================================================
// 统计信息 Key
//
// 只存"创建时间/最后写入时间"这类无法从数据本身推出来的东西；
// 行数是需要时扫出来的（不维护计数器，避免写放大与计数漂移）。
// key 形状与 schema 刻意不同，按前缀删不会误伤。
// ============================================================

// 某个库的统计（创建时间）
inline std::string db_stats(const Identifier &db) {
  return "@system/dbstats/" + encode_identifier(db);
}

// 某张表的统计（创建时间、最后写入时间）
inline std::string table_stats(const Identifier &db, const Identifier &table) {
  return "@system/tablestats/" + encode_identifier(db) + "/" +
         encode_identifier(table);
}

// 某个库下所有表统计的前缀（DROP DATABASE 用）
inline std::string table_stats_prefix(const Identifier &db) {
  return "@system/tablestats/" + encode_identifier(db) + "/";
}

// ============================================================
// 数据 Key
// ============================================================

// 表数据前缀：@data/<db>/<table>/
inline std::string data_prefix(const Identifier &db, const Identifier &table) {
  return "@data/" + encode_identifier(db) + "/" + encode_identifier(table) +
         "/";
}

// 某个库下所有表数据的前缀（DROP DATABASE 用）
inline std::string db_data_prefix(const Identifier &db) {
  return "@data/" + encode_identifier(db) + "/";
}

// 主键 -> 数据 key（保序编码，见文件头）
inline std::string data_key(const Identifier &db, const Identifier &table,
                            const Value &primary_key, DataType pk_type) {
  return data_prefix(db, table) + KeyCodecs::to_key(primary_key, pk_type);
}

// ============================================================
// Key 类型检查
// ============================================================

inline bool is_system_key(std::string_view key) {
  return key.size() >= 8 && key.substr(0, 8) == "@system/";
}

inline bool is_data_key(std::string_view key) {
  return key.size() >= 6 && key.substr(0, 6) == "@data/";
}

} // namespace sql::keys
