// tx_buffer.cpp
#include "tx_buffer.h"

#include <algorithm>
#include <utility>

namespace kv {
namespace {

// 粗略的内存计量（key + value + 每项固定开销），用于"事务太大"的上限检查
size_t entry_cost(size_t key_size, size_t value_size) {
  return key_size + value_size + 64;
}

} // namespace

void TxBuffer::put(const Key &key, const ByteValue &value) {
  TxEntry &entry = view_[key];
  entry.op.kind = OverlayOp::Kind::kValue;
  entry.op.value = value;
  entry.seq = next_seq_++;

  WriteBatch::Op op;
  op.type = WriteBatch::OpType::kPut;
  op.data.key = key;
  op.data.value = value;
  log_.push_back(std::move(op));

  note_bytes(entry_cost(key.size(), value.size()));
}

void TxBuffer::remove(const Key &key) {
  TxEntry &entry = view_[key];
  entry.op.kind = OverlayOp::Kind::kTombstone;
  entry.op.value.clear();
  entry.seq = next_seq_++;

  WriteBatch::Op op;
  op.type = WriteBatch::OpType::kRemove;
  op.data.key = key;
  log_.push_back(std::move(op));

  note_bytes(entry_cost(key.size(), 0));
}

void TxBuffer::remove_range(const Key &begin, const Key &end) {
  range_deletes_.push_back(TxRangeDelete{begin, end, next_seq_++});

  WriteBatch::Op op;
  op.type = WriteBatch::OpType::kRemoveRange;
  op.data.key = begin;
  op.range_end = end;
  log_.push_back(std::move(op));

  note_bytes(entry_cost(begin.size(), end.size()));
}

OverlayOp TxBuffer::lookup(const Key &key) const {
  // 覆盖该 key 的"最后一条操作"有效：可能是按 key 的写/删，
  // 也可能是某次 remove_range（后者的序号更大才算覆盖）。
  const auto entry = view_.find(key);
  const size_t key_seq = entry != view_.end() ? entry->second.seq : 0;
  size_t range_seq = 0;
  for (const TxRangeDelete &range : range_deletes_) {
    if (key >= range.begin && key < range.end && range.seq > range_seq) {
      range_seq = range.seq;
    }
  }
  if (range_seq > key_seq) {
    OverlayOp op;
    op.kind = OverlayOp::Kind::kTombstone;
    return op;
  }
  if (entry == view_.end()) {
    return OverlayOp{};
  }
  return entry->second.op;
}

WriteBatch TxBuffer::to_batch() const {
  WriteBatch batch;
  for (const WriteBatch::Op &op : log_) {
    switch (op.type) {
    case WriteBatch::OpType::kPut:
      batch.put(op.data.key, op.data.value.value_or(ByteValue{}));
      break;
    case WriteBatch::OpType::kRemove:
      batch.remove(op.data.key);
      break;
    case WriteBatch::OpType::kRemoveRange:
      batch.remove_range(op.data.key, op.range_end);
      break;
    }
  }
  return batch;
}

void TxBuffer::clear() {
  view_.clear();
  range_deletes_.clear();
  log_.clear();
  bytes_ = 0;
  next_seq_ = 1;
}

// ============================================================
// OverlayCursor
// ============================================================
OverlayCursor::OverlayCursor(const TxBuffer *buffer, const Key *begin,
                             const Key *end, ScanDirection direction)
    : buffer_(buffer), direction_(direction) {
  if (begin != nullptr) {
    begin_ = *begin;
  }
  if (end != nullptr) {
    end_ = *end;
  }
  if (buffer_ == nullptr) {
    return; // 空游标：valid() 恒 false
  }
  map_end_ = buffer_->view_.end();
  seek_start();
}

bool OverlayCursor::in_bounds() const {
  if (buffer_ == nullptr) {
    return false;
  }
  if (it_ == map_end_) {
    return false;
  }
  if (begin_.has_value() && it_->first < *begin_) {
    return false;
  }
  if (end_.has_value() && !(it_->first < *end_)) {
    return false; // 开区间上界
  }
  return true;
}

bool OverlayCursor::valid() const { return in_bounds(); }

void OverlayCursor::next() {
  if (buffer_ == nullptr || it_ == map_end_) {
    return;
  }
  if (direction_ == ScanDirection::kForward) {
    ++it_;
  } else if (it_ == buffer_->view_.begin()) {
    it_ = map_end_;
  } else {
    --it_;
  }
}

void OverlayCursor::prev() {
  if (buffer_ == nullptr) {
    return;
  }
  if (it_ == map_end_) {
    // 已经在末尾之外：回退到扫描方向上的最后一项
    it_ = buffer_->view_.empty() ? map_end_ : std::prev(map_end_);
    return;
  }
  if (direction_ == ScanDirection::kForward) {
    it_ = (it_ == buffer_->view_.begin()) ? map_end_ : std::prev(it_);
  } else {
    ++it_;
  }
}

const Key &OverlayCursor::key() const { return it_->first; }

OverlayOp OverlayCursor::op() const {
  return buffer_ != nullptr ? buffer_->lookup(it_->first) : OverlayOp{};
}

void OverlayCursor::seek(const Key &key) {
  if (buffer_ == nullptr) {
    return;
  }
  if (direction_ == ScanDirection::kForward) {
    it_ = buffer_->view_.lower_bound(key); // 第一个 >= key
  } else {
    // 反向：最后一个 <= key
    auto it = buffer_->view_.upper_bound(key);
    it_ = (it == buffer_->view_.begin()) ? map_end_ : std::prev(it);
  }
}

void OverlayCursor::seek_start() {
  if (buffer_ == nullptr) {
    return;
  }
  if (direction_ == ScanDirection::kForward) {
    it_ = begin_.has_value() ? buffer_->view_.lower_bound(*begin_)
                             : buffer_->view_.begin();
    return;
  }
  // 反向扫描的起点 = 区间里最大的 key
  if (end_.has_value()) {
    auto it = buffer_->view_.lower_bound(*end_);
    it_ = (it == buffer_->view_.begin()) ? map_end_ : std::prev(it);
  } else {
    it_ = buffer_->view_.empty() ? map_end_ : std::prev(map_end_);
  }
}

void OverlayCursor::seek_finish() {
  if (buffer_ == nullptr) {
    return;
  }
  if (direction_ == ScanDirection::kForward) {
    if (end_.has_value()) {
      auto it = buffer_->view_.lower_bound(*end_);
      it_ = (it == buffer_->view_.begin()) ? map_end_ : std::prev(it);
    } else {
      it_ = buffer_->view_.empty() ? map_end_ : std::prev(map_end_);
    }
    return;
  }
  it_ = begin_.has_value() ? buffer_->view_.lower_bound(*begin_)
                           : buffer_->view_.begin();
}

} // namespace kv
