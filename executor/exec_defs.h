// exec_defs.h
//
// 执行器的错误类型。
//
// 分两类，别混：
//
//   1) **行流错误**：`sql::CursorError`（定义在 sql_types/cursor.h）。
//      取下一行的返回类型统一是
//          std::expected<sql::Row, sql::CursorError>
//      正常结束用 `CursorErrorCode::END` 表示（不是错误），
//      所以不需要 optional + error 两个通道。
//      为什么不用 ExecError：sql_types 不能反向依赖 executor，
//      而 relation 的存储游标和 executor 的算子都要用同一套"流结束"语义，
//      所以这个类型必须住在 sql_types，两边共用。
//
//   2) **执行器错误**：本文件的 ExecError。覆盖"执行这件事本身"失败：
//      打不开/关闭失败、计划不受支持、语句类型不对、资源不足等。
//      它不参与逐行迭代，因此不需要 END。
//
// 约定：库代码不打印；错误码用于控制流，message 用于日志/CLI。
#pragma once

#include <cstdint>
#include <string>
#include <utility>

namespace exec {

enum class ExecErrorCode : uint8_t {
  OK = 0,

  NOT_OPEN, // 执行器没 open，或底层存储没打开
  ALREADY_OPEN,
  INVALID_ARGUMENT, // 计划/表/schema 参数有问题
  TABLE_NOT_FOUND,
  COLUMN_NOT_FOUND,
  UNSUPPORTED_PLAN, // 计划里有本执行器还不支持的节点
  UNSUPPORTED_STMT, // 语句类型不支持（比如 DDL 走别的入口）
  SCHEMA_ERROR,
  MEMORY_LIMIT, // 排序等超出内存上限（外部排序还没做）
  IO_ERROR,
  INTERNAL,
};

inline const char *exec_error_message(ExecErrorCode code) {
  switch (code) {
  case ExecErrorCode::OK:
    return "OK";
  case ExecErrorCode::NOT_OPEN:
    return "Executor is not open";
  case ExecErrorCode::ALREADY_OPEN:
    return "Executor is already open";
  case ExecErrorCode::INVALID_ARGUMENT:
    return "Invalid argument";
  case ExecErrorCode::TABLE_NOT_FOUND:
    return "Table not found";
  case ExecErrorCode::COLUMN_NOT_FOUND:
    return "Column not found";
  case ExecErrorCode::UNSUPPORTED_PLAN:
    return "Unsupported plan node";
  case ExecErrorCode::UNSUPPORTED_STMT:
    return "Unsupported statement";
  case ExecErrorCode::SCHEMA_ERROR:
    return "Row does not match schema";
  case ExecErrorCode::MEMORY_LIMIT:
    return "Memory limit exceeded";
  case ExecErrorCode::IO_ERROR:
    return "Storage I/O error";
  case ExecErrorCode::INTERNAL:
    return "Internal error";
  default:
    return "Unknown error";
  }
}

struct ExecError {
  ExecErrorCode code = ExecErrorCode::OK;
  std::string message;

  ExecError() = default;
  ExecError(ExecErrorCode c, std::string msg)
      : code(c), message(std::move(msg)) {}

  bool ok() const { return code == ExecErrorCode::OK; }
  explicit operator bool() const { return ok(); }

  std::string to_string() const {
    return message.empty() ? std::string(exec_error_message(code)) : message;
  }
};

} // namespace exec
