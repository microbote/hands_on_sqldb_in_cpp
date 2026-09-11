// stmt_defs.h
//
// statement 模块的错误类型。
//
// 设计约定（与 sql_types / parser 一致）：
//   - 可预期的失败用 std::expected 返回，**错误码与错误信息一起作为值返回**，
//     不放在对象状态里 —— 这样 builder/validator 都是无状态的，
//     可以重复调用、并发使用，也不会出现"上一次的错误粘住下一次调用"。
//   - 错误码（StmtErrorCode）用于控制流与测试；
//     错误信息（StmtError::message）用于日志与 CLI，带上下文（表名/列名等）。
#pragma once

#include <cstdint>
#include <string>
#include <utility>

namespace stmt {

enum class StmtErrorCode : uint8_t {
  OK = 0,

  // ---- validator Error (1..49) ----
  UNKNOWN_STMT_TYPE = 1,
  VALIDATOR_NOT_INIT,
  DATABASE_NOT_FOUND,
  DATABASE_ALREADY_EXISTS,
  TABLE_NOT_FOUND,
  TABLE_ALREADY_EXISTS,
  COLUMN_NOT_FOUND,
  COLUMN_COUNT_MISMATCH,
  COLUMN_TYPE_MISMATCH,
  VALUE_OUT_OF_RANGE,
  COLUMN_ATTR_NULL_MISMATCH,
  DUPLICATE_COLUMN,
  DUPLICATE_PRIMARY_KEY,
  NO_PRIMARY_KEY,
  INVALID_SCHEMA,
  EMPTY_STATEMENT, // 语句里没有表名/列名等必要信息

  // ---- builder Error (99..) ----
  AST_IS_NULL = 99,
  UNKNOWN_AST_Type,
  INVALID_AST_NODE,     // 节点结构不完整（缺字段/类型不对）
  UNSUPPORTED_AST_NODE, // 语法存在但本模块暂不支持
};

// 错误码的可读描述
inline const char *stmt_error_message(StmtErrorCode err) {
  switch (err) {
  case StmtErrorCode::OK:
    return "OK";
  case StmtErrorCode::UNKNOWN_STMT_TYPE:
    return "Unknown statement type";
  case StmtErrorCode::VALIDATOR_NOT_INIT:
    return "Validator is not initialized (catalog is not open?)";
  case StmtErrorCode::DATABASE_NOT_FOUND:
    return "Database not found";
  case StmtErrorCode::DATABASE_ALREADY_EXISTS:
    return "Database already exists";
  case StmtErrorCode::TABLE_NOT_FOUND:
    return "Table not found";
  case StmtErrorCode::TABLE_ALREADY_EXISTS:
    return "Table already exists";
  case StmtErrorCode::COLUMN_NOT_FOUND:
    return "Column not found";
  case StmtErrorCode::COLUMN_COUNT_MISMATCH:
    return "Column count mismatch";
  case StmtErrorCode::COLUMN_TYPE_MISMATCH:
    return "Column type mismatch";
  case StmtErrorCode::VALUE_OUT_OF_RANGE:
    return "Value out of range for column type";
  case StmtErrorCode::COLUMN_ATTR_NULL_MISMATCH:
    return "NULL constraint violation";
  case StmtErrorCode::DUPLICATE_COLUMN:
    return "Duplicate column name";
  case StmtErrorCode::DUPLICATE_PRIMARY_KEY:
    return "Duplicate primary key";
  case StmtErrorCode::NO_PRIMARY_KEY:
    return "No primary key defined";
  case StmtErrorCode::INVALID_SCHEMA:
    return "Invalid table schema";
  case StmtErrorCode::EMPTY_STATEMENT:
    return "Statement is missing required information";
  case StmtErrorCode::AST_IS_NULL:
    return "AST is null";
  case StmtErrorCode::UNKNOWN_AST_Type:
    return "Unknown AST node type";
  case StmtErrorCode::INVALID_AST_NODE:
    return "Malformed AST node";
  case StmtErrorCode::UNSUPPORTED_AST_NODE:
    return "Unsupported AST node";
  default:
    return "Unknown error";
  }
}

// 错误值：错误码 + 上下文信息
struct StmtError {
  StmtErrorCode code = StmtErrorCode::OK;
  std::string message;

  StmtError() = default;
  StmtError(StmtErrorCode c, std::string msg)
      : code(c), message(std::move(msg)) {}

  bool ok() const { return code == StmtErrorCode::OK; }
  explicit operator bool() const { return ok(); }

  // message 为空时退回错误码的通用描述
  const std::string &what() const {
    static const std::string empty;
    return message.empty() ? empty : message;
  }

  std::string to_string() const {
    if (!message.empty()) {
      return message;
    }
    return stmt_error_message(code);
  }
};

} // namespace stmt
