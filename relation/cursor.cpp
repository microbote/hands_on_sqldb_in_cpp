// cursor.cpp
#include "cursor.h"

#include <string>
#include <utility>

#include "storage/kv_engine/kv_engine.h"
#include "table.h"

namespace sql {

// relation 的操作错误 -> 行流错误（声明在 relation_defs.h）
CursorError to_cursor_error(const RelError &error) {
  switch (error.code) {
  case RelErrorCode::NOT_FOUND:
    return CursorError(CursorErrorCode::NOT_FOUND, error.to_string());
  case RelErrorCode::SCHEMA_ERROR:
    return CursorError(CursorErrorCode::SCHEMA_ERROR, error.to_string());
  case RelErrorCode::NOT_OPEN:
  case RelErrorCode::KV_ERROR:
    return CursorError(CursorErrorCode::IO_ERROR, error.to_string());
  case RelErrorCode::COLUMN_NOT_FOUND:
  case RelErrorCode::PRIMARY_KEY_NULL:
  case RelErrorCode::PRIMARY_KEY_MISMATCH:
  case RelErrorCode::TABLE_NOT_FOUND:
    return CursorError(CursorErrorCode::INVALID_ARGUMENT, error.to_string());
  default:
    return CursorError(CursorErrorCode::INTERNAL, error.to_string());
  }
}

TableCursor::TableCursor(const Table *table, KeyRange range,
                         std::unique_ptr<kv::Iterator> it)
    : table_(table), range_(std::move(range)), it_(std::move(it)) {}

TableCursor::~TableCursor() = default;

std::expected<Row, CursorError> TableCursor::next() {
  // 已经出过错就把同一个错误再报一次：客户端漏看一次也不会把错误当结束
  if (error_.is_error()) {
    return std::unexpected(error_);
  }
  if (table_ == nullptr || it_ == nullptr) {
    error_ = end_of_stream(); // 空扫描（比如空 KeyRange）也算正常结束
    return std::unexpected(error_);
  }
  if (!it_->valid()) {
    const kv::Status status = it_->status();
    if (status != kv::Status::OK && status != kv::Status::NotFound) {
      error_ = CursorError(CursorErrorCode::IO_ERROR,
                           "kv scan failed: " + it_->error_message());
      return std::unexpected(error_);
    }
    error_ = end_of_stream(); // 扫到区间边界 = 正常结束
    return std::unexpected(error_);
  }

  auto decoded = table_->decode_row(it_->value());
  it_->next(); // 先推进再返回：即使解码失败也不停在同一行
  if (!decoded.has_value()) {
    error_ = to_cursor_error(decoded.error());
    return std::unexpected(error_);
  }
  return std::move(*decoded);
}

void TableCursor::close() {
  it_.reset();
  if (!error_.is_error()) {
    error_ = end_of_stream();
  }
}

void TableCursor::reset() {
  error_ = CursorError();
  if (it_ != nullptr) {
    it_->seek_to_first(); // 迭代器自己知道方向（正向/反向）
  }
}

} // namespace sql
