#include "cursor.h"
#include "table.h"

namespace sql {

// ============================================================
// TableCursor 实现（只负责遍历）
// ============================================================
std::optional<Row> TableCursor::next() {
  if (!has_next() || !table_) {
    return std::nullopt;
  }

  // 通过 Table 解码，Cursor 不关心编码细节
  Row row = table_->decode_row(it_->value());
  it_->next();
  return row;
}

std::string TableCursor::error_message() const {
  if (!table_) return "Table null";
  return (!it_) ? "kv::Iterator null" : it_->error_message();
}

void TableCursor::reset() {
  if (it_) {
    it_->seek_to_first();
  }
}



} // namespace sql
