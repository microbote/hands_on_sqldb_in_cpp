#include "mock_engine.h"

// mock_engine.cpp
#include <iostream>
#include <sstream>

#include "mock_engine.h"

namespace kv {

// ============================================================
// MockIterator 实现
// ============================================================
MockIterator::MockIterator(const std::map<Key, ByteValue> *data,
                           const KeyRange &range)
    : data_(data), range_(range), pos_(data_->end()), status_(Status::OK) {
  if (!data_) {
    status_ = Status::InternalError;
    error_msg_ = "Data pointer is null";
    return;
  }

  // 定位到起始位置
  seek_to_first();
}

void MockIterator::seek(const Key &key) {
  if (!data_) {
    status_ = Status::InternalError;
    error_msg_ = "Data pointer is null";
    return;
  }

  if (range_.direction == ScanDirection::kForward) {
    // 正向：第一个 >= key 的 key；不能跑到 range.start 之前
    Key target = key;
    if (range_.start && target < *range_.start) {
      target = *range_.start;
    }
    pos_ = data_->lower_bound(target); // first of >= key
  } else {
    // 反向扫描：找 <= key 的最后一个；排他上界 range.end 本身不能返回
    auto it = data_->upper_bound(key); // first of > key
    if (it == data_->begin()) {
      pos_ = data_->end();
    } else {
      --it;
      pos_ = it;
    }
    if (pos_ != data_->end() && range_.end && pos_->first >= *range_.end) {
      if (pos_ == data_->begin()) {
        pos_ = data_->end();
      } else {
        --pos_;
      }
    }
  }

  update_status();
}

void MockIterator::seek_to_first() {
  if (!data_) {
    status_ = Status::InternalError;
    error_msg_ = "Data pointer is null";
    return;
  }

  // 必须尊重 range 边界：有界区间不能从整库的第一条/最后一条开始，
  // 否则 seek_to_first() 之后 valid() 立刻为 false，整个扫描扫不到东西。
  // （LevelDBIterator::seek_to_first 是这么做的，这里对齐它的行为）
  if (range_.direction == ScanDirection::kForward) {
    if (range_.start) {
      seek(*range_.start);
      return;
    }
    pos_ = data_->begin();
  } else {
    if (range_.end) {
      seek(*range_.end); // seek() 会把 == end 的那条排掉
      return;
    }
    if (data_->empty()) {
      pos_ = data_->end();
    } else {
      auto it = data_->end();
      --it;
      pos_ = it;
    }
  }

  update_status();
}

void MockIterator::seek_to_last() {
  if (!data_) {
    status_ = Status::InternalError;
    error_msg_ = "Data pointer is null";
    return;
  }

  if (range_.direction == ScanDirection::kForward) {
    // 区间内最后一个 key（注意排他上界）
    if (range_.end) {
      auto it = data_->lower_bound(*range_.end);
      if (it == data_->begin()) {
        pos_ = data_->end();
      } else {
        --it;
        pos_ = it;
      }
      if (pos_ != data_->end() && range_.start && pos_->first < *range_.start) {
        pos_ = data_->end();
      }
    } else if (data_->empty()) {
      pos_ = data_->end();
    } else {
      auto it = data_->end();
      --it;
      pos_ = it;
    }
  } else {
    // 反向迭代器的"最后"= 区间内第一个 key
    if (range_.start) {
      pos_ = data_->lower_bound(*range_.start);
    } else {
      pos_ = data_->begin();
    }
    if (pos_ != data_->end() && range_.end && pos_->first >= *range_.end) {
      pos_ = data_->end();
    }
  }

  update_status();
}

void MockIterator::next() {
  if (!valid())
    return;

  if (range_.direction == ScanDirection::kForward) {
    ++pos_;
  } else {
    if (pos_ == data_->begin()) {
      pos_ = data_->end();
    } else {
      --pos_;
    }
  }

  update_status();
}

void MockIterator::prev() {
  if (!valid())
    return;

  if (range_.direction == ScanDirection::kForward) {
    if (pos_ == data_->begin()) {
      pos_ = data_->end();
    } else {
      --pos_;
    }
  } else {
    ++pos_;
  }

  update_status();
}

bool MockIterator::valid() const {
  if (!data_)
    return false;
  if (status_ != Status::OK)
    return false;
  if (pos_ == data_->end())
    return false;

  // 检查范围
  const Key &key = pos_->first;
  if (range_.start && key < *range_.start)
    return false;
  if (range_.end && key >= *range_.end)
    return false;

  return true;
}

Key MockIterator::key() const {
  if (!valid())
    return "";
  return pos_->first;
}

ByteValue MockIterator::value() const {
  if (!valid())
    return "";
  return pos_->second;
}

KVPair MockIterator::kvpair() const {
  if (!valid())
    return {"", std::nullopt};
  return {pos_->first, pos_->second};
}

Status MockIterator::status() const { return status_; }

std::string MockIterator::error_message() const { return error_msg_; }

void MockIterator::update_status() {
  if (!data_) {
    status_ = Status::InternalError;
    error_msg_ = "Data pointer is null";
    return;
  }

  if (pos_ == data_->end()) {
    status_ = Status::NotFound;
    error_msg_ = "No more data";
    return;
  }

  if (in_range()) {
    status_ = Status::OK;
    error_msg_ = "";
  } else {
    status_ = Status::NotFound;
    error_msg_ = "Out of range";
  }
}

bool MockIterator::in_range() const {
  if (pos_ == data_->end())
    return false;

  const Key &key = pos_->first;
  if (range_.start && key < *range_.start)
    return false;
  if (range_.end && key >= *range_.end)
    return false;

  return true;
}

// ============================================================
// MockEngine 实现
// ============================================================

// ----- 生命周期 -----
Status MockEngine::open_database(DatabaseOptions options) {
  std::lock_guard<std::mutex> lock(mutex_);

  if (is_open_) {
    return Status::AlreadyExists;
  }

  options_ = options;
  is_open_ = true;

  return Status::OK;
}

Status MockEngine::close_database() {
  std::lock_guard<std::mutex> lock(mutex_);

  if (!is_open_) {
    return Status::NotFound;
  }

  is_open_ = false;
  // 可以选择清空数据或保留
  // data_.clear();

  return Status::OK;
}

bool MockEngine::is_open() const { return is_open_.load(); }

// ----- 单条操作 -----
Status MockEngine::get(const Key &key, ByteValue *value) {
  if (!is_open_) {
    return Status::InternalError;
  }

  std::lock_guard<std::mutex> lock(mutex_);

  // 事务读穿：本事务写过就返回自己的值，删过就是不存在
  if (tx_ != nullptr) {
    const OverlayOp op = tx_->lookup(key);
    if (op.is_tombstone()) {
      return Status::NotFound;
    }
    if (op.has_value()) {
      if (value) {
        *value = op.value;
      }
      return Status::OK;
    }
  }

  auto it = data_.find(key);
  if (it == data_.end()) {
    return Status::NotFound;
  }

  if (value) {
    *value = it->second;
  }

  return Status::OK;
}

Status MockEngine::put(const Key &key, const ByteValue &value) {
  if (!is_open_) {
    return Status::InternalError;
  }

  std::lock_guard<std::mutex> lock(mutex_);

  if (tx_ != nullptr) {
    tx_->put(key, value);
    return Status::OK;
  }

  data_[key] = value;
  return Status::OK;
}

Status MockEngine::remove(const Key &key) {
  if (!is_open_) {
    return Status::InternalError;
  }

  std::lock_guard<std::mutex> lock(mutex_);

  if (tx_ != nullptr) {
    tx_->remove(key);
    return Status::OK; // 事务里删不存在的 key 也算成功（提交时是幂等的）
  }

  auto it = data_.find(key);
  if (it == data_.end()) {
    return Status::NotFound;
  }

  data_.erase(it);
  return Status::OK;
}

bool MockEngine::exists(const Key &key) {
  if (!is_open_) {
    return false;
  }

  std::lock_guard<std::mutex> lock(mutex_);
  if (tx_ != nullptr) {
    const OverlayOp op = tx_->lookup(key);
    if (op.covered()) {
      return op.has_value();
    }
  }
  return data_.find(key) != data_.end();
}

// ----- 批量操作 -----
Status MockEngine::get_batch(const std::vector<Key> &keys,
                             MissingKeyPolicy policy,
                             std::vector<std::optional<ByteValue>> *values) {
  if (!is_open_) {
    return Status::InternalError;
  }

  if (!values) {
    return Status::InvalidArgument;
  }

  std::lock_guard<std::mutex> lock(mutex_);

  values->clear();
  values->reserve(keys.size());

  for (const auto &key : keys) {
    auto it = data_.find(key);
    if (it == data_.end()) {
      if (policy == MissingKeyPolicy::kReturnError) {
        return Status::NotFound;
      }
      values->push_back(std::nullopt);
    } else {
      values->push_back(it->second);
    }
  }

  return Status::OK;
}

Status MockEngine::write_batch(const WriteBatch &batch) {
  if (!is_open_) {
    return Status::InternalError;
  }

  std::lock_guard<std::mutex> lock(mutex_);

  // 事务里再写批量：路由进缓冲（同一个事务里仍然保持顺序与原子性）
  if (tx_ != nullptr) {
    for (const auto &op : batch.ops()) {
      switch (op.type) {
      case WriteBatch::OpType::kPut:
        if (op.data.value.has_value()) {
          tx_->put(op.data.key, op.data.value.value());
        }
        break;
      case WriteBatch::OpType::kRemove:
        tx_->remove(op.data.key);
        break;
      case WriteBatch::OpType::kRemoveRange:
        tx_->remove_range(op.data.key, op.range_end);
        break;
      }
    }
    return Status::OK;
  }

  return apply_batch_locked(batch);
}

// 直接落到 data_（不经过事务缓冲）。调用方必须已经持有 mutex_。
Status MockEngine::apply_batch_locked(const WriteBatch &batch) {
  if (fail_writes_) {
    return Status::IOError;
  }
  // 两趟：先整体校验，再整体应用 —— 这样"任何一条 op 非法"时
  // 一条都不会落下（和 LevelDB 的 WriteBatch 原子性对齐）。
  for (const auto &op : batch.ops()) {
    if (op.type == WriteBatch::OpType::kPut && !op.data.value.has_value()) {
      return Status::InvalidArgument;
    }
    if (op.type == WriteBatch::OpType::kRemoveRange &&
        op.range_end <= op.data.key) {
      return Status::InvalidArgument;
    }
  }
  for (const auto &op : batch.ops()) {
    if (op.type == WriteBatch::OpType::kPut) {
      data_[op.data.key] = op.data.value.value();
    } else if (op.type == WriteBatch::OpType::kRemoveRange) {
      // [begin, end) 整段删除（DROP / TRUNCATE 用）
      auto it = data_.lower_bound(op.data.key);
      while (it != data_.end() && it->first < op.range_end) {
        it = data_.erase(it);
      }
    } else { // kRemove
      data_.erase(op.data.key);
    }
  }
  return Status::OK;
}

// ----- 迭代器 -----
std::unique_ptr<Iterator> MockEngine::new_iterator(const KeyRange &range) {
  if (!is_open_) {
    return nullptr;
  }

  std::lock_guard<std::mutex> lock(mutex_);
  auto inner = std::make_unique<MockIterator>(&data_, range);
  if (tx_ == nullptr) {
    return inner;
  }
  // 事务里扫描：过滤掉本事务删掉的 key、覆盖本事务改过的值
  return std::make_unique<MergingIterator>(std::move(inner), tx_.get(), range);
}

// ----- 事务 -----
Status MockEngine::begin_transaction() {
  if (!is_open_) {
    return Status::InternalError;
  }
  std::lock_guard<std::mutex> lock(mutex_);
  if (tx_ != nullptr) {
    return Status::Busy; // 悲观单写者：同时只允许一个写事务
  }
  tx_ = std::make_unique<TxBuffer>();
  return Status::OK;
}

Status MockEngine::commit_transaction() {
  if (!is_open_) {
    return Status::InternalError;
  }
  std::lock_guard<std::mutex> lock(mutex_);
  if (tx_ == nullptr) {
    return Status::NotFound;
  }
  if (tx_->exceeds()) {
    return Status::InvalidArgument; // 事务太大（调用方应 rollback）
  }
  // 先把缓冲摘下来：apply 阶段不能再走"进缓冲"那条路
  std::unique_ptr<TxBuffer> tx = std::move(tx_);
  WriteBatch batch = tx->to_batch();
  batch.set_sync(true);
  const Status status = apply_batch_locked(batch);
  if (status != Status::OK) {
    tx_ = std::move(tx); // 失败：缓冲留着，调用方可以重试或回滚
    return status;
  }
  return Status::OK; // tx 析构 = 丢弃缓冲
}

Status MockEngine::rollback_transaction() {
  if (!is_open_) {
    return Status::InternalError;
  }
  std::lock_guard<std::mutex> lock(mutex_);
  if (tx_ == nullptr) {
    return Status::NotFound;
  }
  tx_.reset(); // DB 从没被动过：这就是完整的回滚
  return Status::OK;
}

// ----- 管理 -----
void MockEngine::flush() {
  // Mock 引擎不需要 flush
}

std::string MockEngine::stats() const {
  std::lock_guard<std::mutex> lock(mutex_);
  std::ostringstream oss;
  oss << "MockEngine Stats:\n";
  oss << "  is_open: " << (is_open_ ? "true" : "false") << "\n";
  oss << "  entries: " << data_.size() << "\n";
  oss << "  path: " << options_.path << "\n";
  return oss.str();
}

// ----- 测试辅助 -----
void MockEngine::clear() {
  std::lock_guard<std::mutex> lock(mutex_);
  data_.clear();
}

size_t MockEngine::size() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return data_.size();
}

void MockEngine::dump() const {
  std::lock_guard<std::mutex> lock(mutex_);
  std::cout << "MockEngine dump (" << data_.size() << " entries):\n";
  for (const auto &[key, value] : data_) {
    std::cout << "  " << key << " -> " << value << "\n";
  }
}

} // namespace kv
