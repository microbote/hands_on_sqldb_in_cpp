// merging_iterator.h
//
// MergingIterator：事务内的扫描迭代器 —— 把"底层 DB 迭代器"和
// "事务覆盖游标"归并成一个有序行流：
//
//   - 本事务新写的 key（DB 里还没有）  -> 按序**插进去**（这是它比
//     单纯的"过滤包装"多出来的能力，也是多语句事务能读到自己的关键）；
//   - 本事务删掉的 key                 -> 两边都跳过（墓碑吞掉）；
//   - 本事务改过的 key                 -> 用新值覆盖 DB 的旧值；
//   - 其余                             -> 透传 DB。
//
// 方向：两个源都遵循 Iterator 的约定 —— next() 就是"按扫描方向前进"，
// 所以归并只需要把"谁更靠前"的比较反过来（kForward 用 <，kReverse 用 >）。
#pragma once

#include <memory>

#include "kv_engine.h"
#include "tx_buffer.h"

namespace kv {

class MergingIterator : public Iterator {
public:
  MergingIterator(std::unique_ptr<Iterator> db, const TxBuffer *buffer,
                  const KeyRange &range);
  ~MergingIterator() override = default;

  // ----- 定位 -----
  void seek(const Key &key) override;
  void seek_to_first() override;
  void seek_to_last() override;

  // ----- 移动 -----
  void next() override;
  void prev() override; // 与 next() 相反的方向（Iterator 的约定）

  // ----- 状态 -----
  bool valid() const override { return valid_; }
  Key key() const override { return valid_ ? current_key_ : Key(); }
  ByteValue value() const override {
    return valid_ ? current_value_ : ByteValue();
  }
  KVPair kvpair() const override;
  Status status() const override;
  std::string error_message() const override;

private:
  // 归并一步：决定当前项（并跳过墓碑），必要时循环到下一个
  void settle();
  // 当前项来自哪个源（next() 时要推进对应的源）；settle() 末尾会反推出来
  void advance_sources();
  // "扫描方向上严格在 key 之前的那一项"（跳过被墓碑吞掉的 key）
  bool predecessor_of(const Key &key, Key *out_key, ByteValue *out_value);
  // 扫描方向的最后一项（forward -> 最大 key；reverse -> 最小 key）
  bool last_item(Key *out_key, ByteValue *out_value);
  // 把两个源定位到 key（或其之后），交给 settle() 产出这一项
  void reposition_to(const Key &key);
  bool overlay_first() const; // 覆盖层的 key 是否排在 DB 的 key 前面
  // 两个候选里"在扫描方向上更靠后"的那个（forward 取大、reverse 取小）
  static bool later_in_scan(ScanDirection direction, const Key &a,
                            const Key &b) {
    return direction == ScanDirection::kForward ? a > b : a < b;
  }

  std::unique_ptr<Iterator> db_;
  const TxBuffer *buffer_ = nullptr;
  OverlayCursor overlay_;
  ScanDirection direction_ = ScanDirection::kForward;

  bool valid_ = false;
  Key current_key_;
  ByteValue current_value_;
  bool from_db_ = false;
  bool from_overlay_ = false;
};

} // namespace kv
