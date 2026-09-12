// sql_types/cursor.h
//
// Cursor：行流接口（客户端只看这一个抽象）。
//
// 为什么放在 sql_types 而不是 relation：
//   - 它是"结果行流"的抽象，与存储方式无关：存储扫描可以实现它，
//     执行器的根算子（排序/过滤/投影之后的最终行流）也可以实现它；
//   - 客户端（cmdline / 未来的驱动）只需要依赖 sql_types 就能拿到它，
//     不必去依赖具体存储层。
//
// 迭代约定（只有两条，够用且不留歧义）：
//   next() -> std::expected<Row, CursorError>
//     有值            : 拿到一行
//     CursorErrorCode::END : **正常结束**（不是错误）
//     其它错误码      : 出错，且错误会"粘住"——再次调用下次仍返回同一个错误，
//                       客户端不会因为漏看一次返回值而把错误当成正常结束。
//   close() -> 释放底层资源（KV 迭代器/排序缓冲），幂等；析构时也应调用。
//
// 以前的 valid()/reset()/range() 没有放在这里：valid() 需要预读一行才知道答案，
// reset()/range() 是"存储扫描"特有的概念 —— 它们下沉到实现类
// （见表 TableCursor）。接口只放所有实现者都成立的成员。
#pragma once

#include <cstdint>
#include <expected>
#include <string>
#include <utility>

#include "row.h"

namespace sql {

enum class CursorErrorCode : uint8_t {
  OK = 0,
  END, // 流正常结束（不是错误）
  NOT_FOUND,
  SCHEMA_ERROR, // 行与 schema 不匹配（数据损坏）
  INVALID_ARGUMENT,
  IO_ERROR,
  INTERNAL,
};

inline const char *cursor_error_message(CursorErrorCode code) {
  switch (code) {
  case CursorErrorCode::OK:
    return "OK";
  case CursorErrorCode::END:
    return "End of stream";
  case CursorErrorCode::NOT_FOUND:
    return "Row not found";
  case CursorErrorCode::SCHEMA_ERROR:
    return "Row does not match schema";
  case CursorErrorCode::INVALID_ARGUMENT:
    return "Invalid argument";
  case CursorErrorCode::IO_ERROR:
    return "Storage I/O error";
  case CursorErrorCode::INTERNAL:
    return "Internal error";
  default:
    return "Unknown error";
  }
}

struct CursorError {
  CursorErrorCode code = CursorErrorCode::OK;
  std::string message;

  CursorError() = default;
  CursorError(CursorErrorCode c, std::string msg)
      : code(c), message(std::move(msg)) {}

  // 是否正常结束（END 不是错误）
  bool end() const { return code == CursorErrorCode::END; }

  // 是否出错（OK 与 END 都不算出错）
  bool is_error() const {
    return code != CursorErrorCode::OK && code != CursorErrorCode::END;
  }

  std::string to_string() const {
    return message.empty() ? std::string(cursor_error_message(code)) : message;
  }
};

// 正常结束的返回值
inline CursorError end_of_stream() {
  return CursorError(CursorErrorCode::END, "");
}

class Cursor {
public:
  virtual ~Cursor() = default;

  // 取下一行；流结束返回 CursorError{END, ""}，出错返回其它错误码
  virtual std::expected<Row, CursorError> next() = 0;

  // 释放底层资源；幂等（重复调用/未 open 就调用都安全）
  virtual void close() = 0;
};

} // namespace sql
