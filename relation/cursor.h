#pragma once

#include <optional>
#include "primary_key_range.h"
#include "storage/kv_engine/kv_engine.h"

namespace sql {
  class Row;
  class Table;
// ============================================================
// 游标接口（Volcano 模型）
// ============================================================
class Cursor {
   public:
    virtual ~Cursor() = default;

    // 获取下一行
    virtual std::optional<Row> next() = 0;

    // 检查是否还有数据
    virtual bool has_next() const = 0;

    // 重置游标到起始位置
    virtual void reset() = 0;

    virtual PrimaryKeyRange range() const = 0;

    virtual std::string error_message() const = 0;

    virtual bool valid() const = 0;
  };

  // ============================================================
  // TableCursor 实现
  // ============================================================
  class TableCursor : public Cursor {
   public:
    // 传入 Table 指针，通过 Table 进行所有转换
    TableCursor(const Table* table, const PrimaryKeyRange& range,
                std::unique_ptr<kv::Iterator> it)
                :table_(table), range_(range), it_(std::move(it)) {};

    std::optional<Row> next() override;

    bool has_next() const override {
      return valid();
    }
    void reset() override;

    PrimaryKeyRange range() const override { return range_; }

    virtual std::string error_message() const override;

    bool valid() const override { return table_ && it_ && it_->valid(); }

   private:
    const Table* table_;  // 通过 Table 访问编码/解码能力
    PrimaryKeyRange range_;
    std::unique_ptr<kv::Iterator> it_;
  };
}