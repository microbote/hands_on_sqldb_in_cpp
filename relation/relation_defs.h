// relation_defs.h
//
// relation 层（SQL 关系建模层）的错误类型。
//
// 设计约定与其它模块一致：
//   - 可预期的失败用 std::expected 返回，**错误码 + 错误信息一起作为值返回**，
//     不放在对象状态里（Table/Cursor 都是无状态或一次性对象，可重复使用）；
//   - 错误码用于控制流与测试，message 用于日志/CLI（带库表列名等上下文）；
//   - 库代码不打印：需要输出的调用方自己拿 message 去打印。
#pragma once

#include <cstdint>
#include <string>
#include <utility>

#include "sql_types/cursor.h"

namespace sql {

enum class RelErrorCode : uint8_t {
  OK = 0,

  NOT_OPEN,             // KV 引擎没有打开
  TABLE_NOT_FOUND,      // 表不存在（schema 查不到）
  NOT_FOUND,            // 行不存在
  SCHEMA_ERROR,         // 行/schema 校验失败，细节在 SchemaError 里
  PRIMARY_KEY_NULL,     // 主键是 NULL
  PRIMARY_KEY_MISMATCH, // 更新时行里的主键和 key 不一致
  COLUMN_NOT_FOUND,
  KV_ERROR, // 底层 KV 返回非 OK
  NOT_SUPPORTED,
};

inline const char *rel_error_message(RelErrorCode code) {
  switch (code) {
  case RelErrorCode::OK:
    return "OK";
  case RelErrorCode::NOT_OPEN:
    return "KV engine is not open";
  case RelErrorCode::TABLE_NOT_FOUND:
    return "Table not found";
  case RelErrorCode::NOT_FOUND:
    return "Row not found";
  case RelErrorCode::SCHEMA_ERROR:
    return "Row does not match table schema";
  case RelErrorCode::PRIMARY_KEY_NULL:
    return "Primary key is NULL";
  case RelErrorCode::PRIMARY_KEY_MISMATCH:
    return "Primary key in row does not match the key";
  case RelErrorCode::COLUMN_NOT_FOUND:
    return "Column not found";
  case RelErrorCode::KV_ERROR:
    return "KV engine error";
  case RelErrorCode::NOT_SUPPORTED:
    return "Not supported";
  default:
    return "Unknown error";
  }
}

struct RelError {
  RelErrorCode code = RelErrorCode::OK;
  std::string message;

  RelError() = default;
  RelError(RelErrorCode c, std::string msg)
      : code(c), message(std::move(msg)) {}

  bool ok() const { return code == RelErrorCode::OK; }
  explicit operator bool() const { return ok(); }

  std::string to_string() const {
    return message.empty() ? std::string(rel_error_message(code)) : message;
  }
};

// relation 的操作错误 -> 行流错误（Cursor 用）。放在这里是为了让上层
// （executor）也能把 Table 操作的失败折叠成 CursorError，不必各写一份映射。
CursorError to_cursor_error(const RelError& error);

} // namespace sql
