// cursor.h
//
// TableCursor：存储层的行流实现 —— 一次 KV 区间扫描。
//
// 它实现 sql::Cursor（next/close），另外**额外**提供 reset()/range()：
// 这两个是"存储扫描"特有的能力（能从头再扫一遍、知道自己扫的是哪个区间），
// 结果游标不需要它们，所以不进公共接口。
//
// 生命周期：TableCursor 不拥有 Table，只借它的解码能力 ——
// 调用方要保证 Table 比游标活得久（通常由执行器持有 Table）。
#pragma once

#include <memory>

#include "storage/kv_engine/kv_engine.h" // kv::Status（构造时的打开结果）

#include "relation_defs.h"
#include "sql_types/cursor.h"
#include "sql_types/key_range.h"
#include "sql_types/row.h"

namespace kv {
class Iterator;
}

namespace sql {

class Table;

class TableCursor : public Cursor {
public:
  TableCursor(const Table *table, KeyRange range,
              std::unique_ptr<kv::Iterator> it,
              kv::Status open_status = kv::Status::OK);
  // 析构放在 .cpp：unique_ptr<kv::Iterator> 的删除需要完整类型
  ~TableCursor() override;

  // 取下一行：有值 / END / 错误（错误会粘住）
  std::expected<Row, CursorError> next() override;

  // 释放 KV 迭代器；幂等
  void close() override;

  // ---- 存储扫描特有的能力 ----

  // 复位到起点（方向由创建时的 ascending 决定）
  void reset();

  // 本次扫描的逻辑主键范围（调试/测试用）
  const KeyRange &range() const { return range_; }

private:
  const Table *table_; // 借 Table 的解码能力；不持有所有权
  KeyRange range_;
  std::unique_ptr<kv::Iterator> it_;
  // new_iterator() 返回 nullptr 时的原因（NotLeader / Timeout / ...）：
  // nullptr 迭代器绝不能当成"空表"，否则 follower 上的 SELECT 会静默回空集。
  kv::Status open_status_ = kv::Status::OK;
  bool closed_ = false; // close() 之后 = 正常结束（不是"打不开存储"）
  CursorError error_; // 出错后粘住，供后续 next() 重复返回
};

} // namespace sql
