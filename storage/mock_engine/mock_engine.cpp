// mock_engine.cpp
#include "mock_engine.h"

#include <algorithm>
#include <iostream>
#include <sstream>
#include <utility>

namespace kv {

namespace {

// rows 是升序的键数组：第一个 >= key 的下标 / 第一个 > key 的下标
size_t lower_bound_index(const std::vector<KVPair> &rows, const Key &key) {
  auto it = std::lower_bound(
      rows.begin(), rows.end(), key,
      [](const KVPair &row, const Key &target) { return row.key < target; });
  return static_cast<size_t>(it - rows.begin());
}

size_t upper_bound_index(const std::vector<KVPair> &rows, const Key &key) {
  auto it = std::upper_bound(
      rows.begin(), rows.end(), key,
      [](const Key &target, const KVPair &row) { return target < row.key; });
  return static_cast<size_t>(it - rows.begin());
}

} // namespace

// ============================================================
// MockIterator：物化扫描（见头文件说明）
// ============================================================
MockIterator::MockIterator(std::vector<KVPair> rows, const KeyRange &range)
    : rows_(std::move(rows)), range_(range), status_(Status::OK) {
  // 构造完就有当前位置（和 LevelDBIterator 一致）
  seek_to_first();
}

void MockIterator::seek(const Key &key) {
  if (range_.direction == ScanDirection::kForward) {
    // 正向：第一个 >= key；不能跑到 range.start 之前
    Key target = key;
    if (range_.start && target < *range_.start) {
      target = *range_.start;
    }
    index_ = lower_bound_index(rows_, target);
  } else {
    // 反向：最大的 <= key（rows_ 里只装区间内的行）
    const size_t after = upper_bound_index(rows_, key);
    index_ = (after == 0) ? rows_.size() : after - 1;
  }
  update_status();
}

void MockIterator::seek_to_first() {
  if (rows_.empty()) {
    index_ = 0;
    update_status();
    return;
  }
  // "第一"是扫描方向上的第一：正向 = 最小的键，反向 = 最大的键
  index_ = (range_.direction == ScanDirection::kForward) ? 0 : rows_.size() - 1;
  update_status();
}

void MockIterator::seek_to_last() {
  if (rows_.empty()) {
    index_ = 0;
    update_status();
    return;
  }
  index_ = (range_.direction == ScanDirection::kForward) ? rows_.size() - 1 : 0;
  update_status();
}

void MockIterator::next() {
  if (!valid()) {
    return;
  }
  if (range_.direction == ScanDirection::kForward) {
    ++index_; // 越过末尾 -> index_ == rows_.size() -> 无效
  } else {
    index_ = (index_ == 0) ? rows_.size() : index_ - 1;
  }
  update_status();
}

void MockIterator::prev() {
  if (!valid()) {
    return;
  }
  if (range_.direction == ScanDirection::kForward) {
    index_ = (index_ == 0) ? rows_.size() : index_ - 1;
  } else {
    ++index_;
  }
  update_status();
}

bool MockIterator::valid() const {
  if (status_ != Status::OK) {
    return false;
  }
  return in_range(index_);
}

Key MockIterator::key() const { return valid() ? rows_[index_].key : ""; }

ByteValue MockIterator::value() const {
  return valid() ? rows_[index_].value.value_or(ByteValue()) : ByteValue();
}

KVPair MockIterator::kvpair() const {
  if (!valid()) {
    return {"", std::nullopt};
  }
  return rows_[index_];
}

Status MockIterator::status() const { return status_; }

std::string MockIterator::error_message() const { return error_msg_; }

void MockIterator::update_status() {
  if (index_ >= rows_.size()) {
    status_ = Status::NotFound;
    error_msg_ = "No more data";
    return;
  }
  if (in_range(index_)) {
    status_ = Status::OK;
    error_msg_ = "";
  } else {
    status_ = Status::NotFound;
    error_msg_ = "Out of range";
  }
}

bool MockIterator::in_range(size_t index) const {
  if (index >= rows_.size()) {
    return false;
  }
  const Key &key = rows_[index].key;
  if (range_.start && key < *range_.start) {
    return false;
  }
  if (range_.end && key >= *range_.end) {
    return false;
  }
  return true;
}

// ============================================================
// MockStore：存储本体（进程内一份）
// ============================================================

// ----- 生命周期 -----
Status MockStore::open(const DatabaseOptions &options) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (is_open_) {
    return Status::AlreadyExists;
  }
  options_ = options;
  is_open_ = true;
  return Status::OK;
}

Status MockStore::close() {
  std::lock_guard<std::mutex> lock(mutex_);
  if (!is_open_) {
    return Status::NotFound;
  }
  is_open_ = false;
  write_owner_ = nullptr; // 存储关了，写槽一并失效
  return Status::OK;
}

bool MockStore::is_open() const { return is_open_.load(); }

std::shared_ptr<KVEngine> MockStore::connect() {
  return std::make_shared<MockEngine>(shared_from_this());
}

// ----- 写槽（悲观单写者）-----
Status MockStore::acquire_write_slot(const void *owner) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (!is_open_) {
    return Status::InternalError;
  }
  if (write_owner_ != nullptr && write_owner_ != owner) {
    return Status::Busy; // 另一条连接正在写
  }
  write_owner_ = owner;
  return Status::OK;
}

Status MockStore::release_write_slot(const void *owner) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (write_owner_ != owner) {
    return Status::NotFound; // 不是这条连接持有的（或根本没人持有）
  }
  write_owner_ = nullptr;
  return Status::OK;
}

bool MockStore::write_slot_held() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return write_owner_ != nullptr;
}

// ----- 单条操作（不看事务缓冲：那是连接的事）-----
Status MockStore::get(const Key &key, ByteValue *value) const {
  if (!is_open_) {
    return Status::InternalError;
  }
  std::lock_guard<std::mutex> lock(mutex_);
  auto it = data_.find(key);
  if (it == data_.end()) {
    return Status::NotFound;
  }
  if (value) {
    *value = it->second;
  }
  return Status::OK;
}

Status MockStore::put(const Key &key, const ByteValue &value) {
  if (!is_open_) {
    return Status::InternalError;
  }
  std::lock_guard<std::mutex> lock(mutex_);
  data_[key] = value;
  return Status::OK;
}

Status MockStore::remove(const Key &key) {
  if (!is_open_) {
    return Status::InternalError;
  }
  std::lock_guard<std::mutex> lock(mutex_);
  auto it = data_.find(key);
  if (it == data_.end()) {
    return Status::NotFound;
  }
  data_.erase(it);
  return Status::OK;
}

bool MockStore::exists(const Key &key) const {
  if (!is_open_) {
    return false;
  }
  std::lock_guard<std::mutex> lock(mutex_);
  return data_.find(key) != data_.end();
}

// ----- 批量操作 -----
Status
MockStore::get_batch(const std::vector<Key> &keys, MissingKeyPolicy policy,
                     std::vector<std::optional<ByteValue>> *values) const {
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

Status MockStore::write_batch(const WriteBatch &batch) {
  if (!is_open_) {
    return Status::InternalError;
  }
  std::lock_guard<std::mutex> lock(mutex_);
  return apply_batch_locked(batch);
}

// 直接落到 data_。调用方必须已经持有 mutex_。
Status MockStore::apply_batch_locked(const WriteBatch &batch) {
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

// ----- 扫描底座 -----
//
// 把区间内的行**物化**成一份快照：之后迭代器不再碰 map，所以
//   - 扫描期间别的连接提交不影响这条扫描；
//   - 长扫描不持有锁，不会把写者的提交挡在外面。
// Mock 是测试双（表都不大），这份拷贝换来的语义和 LevelDB 一致。
std::unique_ptr<Iterator> MockStore::new_iterator(const KeyRange &range) {
  if (!is_open_) {
    return nullptr;
  }

  std::lock_guard<std::mutex> lock(mutex_);
  std::vector<KVPair> rows;
  auto it = range.start ? data_.lower_bound(*range.start) : data_.begin();
  while (it != data_.end()) {
    if (range.end && it->first >= *range.end) {
      break;
    }
    rows.push_back(KVPair{it->first, it->second});
    ++it;
  }
  return std::make_unique<MockIterator>(std::move(rows), range);
}

// ----- 管理 -----
std::string MockStore::stats() const {
  std::lock_guard<std::mutex> lock(mutex_);
  std::ostringstream oss;
  oss << "MockStore Stats:\n";
  oss << "  is_open: " << (is_open_ ? "true" : "false") << "\n";
  oss << "  entries: " << data_.size() << "\n";
  oss << "  path: " << options_.path << "\n";
  return oss.str();
}

// ----- 测试辅助 -----
void MockStore::clear() {
  std::lock_guard<std::mutex> lock(mutex_);
  data_.clear();
}

size_t MockStore::size() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return data_.size();
}

void MockStore::dump() const {
  std::lock_guard<std::mutex> lock(mutex_);
  std::cout << "MockStore dump (" << data_.size() << " entries):\n";
  for (const auto &[key, value] : data_) {
    std::cout << "  " << key << " -> " << value << "\n";
  }
}

// ============================================================
// MockEngine：一条连接（= 一个 session 的存储视角）
// ============================================================
Status MockEngine::get(const Key &key, ByteValue *value) {
  if (!store_->is_open()) {
    return Status::InternalError;
  }
  // 事务读穿：本连接写过就返回自己的值，删过就是不存在
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
  return store_->get(key, value);
}

Status MockEngine::put(const Key &key, const ByteValue &value) {
  if (!store_->is_open()) {
    return Status::InternalError;
  }
  if (tx_ != nullptr) {
    tx_->put(key, value);
    return Status::OK;
  }
  // 没有显式事务 = 自动提交写：短暂占用写槽，保证"同一时刻只有一个写者"
  // （会话的写语句本来就包在事务里，这条只是让引擎级调用也守同一条规则）
  const Status acquired = store_->acquire_write_slot(this);
  if (acquired != Status::OK) {
    return acquired;
  }
  const Status status = store_->put(key, value);
  store_->release_write_slot(this);
  return status;
}

Status MockEngine::remove(const Key &key) {
  if (!store_->is_open()) {
    return Status::InternalError;
  }
  if (tx_ != nullptr) {
    tx_->remove(key);
    return Status::OK; // 事务里删不存在的 key 也算成功（提交时是幂等的）
  }
  const Status acquired = store_->acquire_write_slot(this);
  if (acquired != Status::OK) {
    return acquired;
  }
  const Status status = store_->remove(key);
  store_->release_write_slot(this);
  return status;
}

bool MockEngine::exists(const Key &key) {
  if (!store_->is_open()) {
    return false;
  }
  if (tx_ != nullptr) {
    const OverlayOp op = tx_->lookup(key);
    if (op.covered()) {
      return op.has_value();
    }
  }
  return store_->exists(key);
}

Status MockEngine::get_batch(const std::vector<Key> &keys,
                             MissingKeyPolicy policy,
                             std::vector<std::optional<ByteValue>> *values) {
  if (!store_->is_open()) {
    return Status::InternalError;
  }
  if (!values) {
    return Status::InvalidArgument;
  }
  // 事务里逐键走"连接视角"（缓冲优先），和单键 get 保持一致
  if (tx_ == nullptr) {
    return store_->get_batch(keys, policy, values);
  }

  values->clear();
  values->reserve(keys.size());
  for (const auto &key : keys) {
    ByteValue value;
    const Status status = get(key, &value);
    if (status == Status::OK) {
      values->push_back(value);
    } else if (status == Status::NotFound) {
      if (policy == MissingKeyPolicy::kReturnError) {
        return Status::NotFound;
      }
      values->push_back(std::nullopt);
    } else {
      return status;
    }
  }
  return Status::OK;
}

Status MockEngine::write_batch(const WriteBatch &batch) {
  if (!store_->is_open()) {
    return Status::InternalError;
  }

  // 事务里再写批量：路由进缓冲（同一个事务里仍然保持顺序与原子性）
  if (tx_ == nullptr) {
    if (batch.empty()) {
      return Status::OK;
    }
    const Status acquired = store_->acquire_write_slot(this);
    if (acquired != Status::OK) {
      return acquired;
    }
    const Status status = store_->write_batch(batch);
    store_->release_write_slot(this);
    return status;
  }
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

std::unique_ptr<Iterator> MockEngine::new_iterator(const KeyRange &range) {
  if (!store_->is_open()) {
    return nullptr;
  }
  auto inner = store_->new_iterator(range); // 物化底座（创建时固定版本）
  if (inner == nullptr) {
    return nullptr;
  }
  if (tx_ == nullptr) {
    return inner;
  }
  // 事务里扫描：过滤掉本事务删掉的 key、覆盖本事务改过的值
  return std::make_unique<MergingIterator>(std::move(inner), tx_.get(), range);
}

// ----- 事务 -----
Status MockEngine::begin_transaction() {
  if (!store_->is_open()) {
    return Status::InternalError;
  }
  if (tx_ != nullptr) {
    return Status::Busy; // 同一条连接重复 begin
  }
  const Status acquired = store_->acquire_write_slot(this);
  if (acquired != Status::OK) {
    return acquired; // 另一条连接正在写 -> Busy（悲观单写者）
  }
  tx_ = std::make_unique<TxBuffer>();
  return Status::OK;
}

Status MockEngine::commit_transaction() {
  if (!store_->is_open()) {
    return Status::InternalError;
  }
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
  const Status status = store_->write_batch(batch);
  if (status != Status::OK) {
    tx_ = std::move(tx); // 失败：缓冲留着，调用方可以重试或回滚
    return status;       // 写槽也还握着（事务还没结束）
  }
  tx_.reset(); // 提交成功：丢弃缓冲
  store_->release_write_slot(this);
  return Status::OK;
}

Status MockEngine::rollback_transaction() {
  if (!store_->is_open()) {
    return Status::InternalError;
  }
  if (tx_ == nullptr) {
    return Status::NotFound;
  }
  tx_.reset(); // DB 从没被动过：这就是完整的回滚
  store_->release_write_slot(this);
  return Status::OK;
}

std::string MockEngine::stats() const { return store_->stats(); }

} // namespace kv
