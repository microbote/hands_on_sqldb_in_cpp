// leveldb_engine.cpp
#include "leveldb_engine.h"

#include <leveldb/cache.h>
#include <leveldb/filter_policy.h>

#include <iostream>
#include <sstream>

namespace kv {

// ============================================================
// LevelDBIterator 实现
// ============================================================

LevelDBIterator::LevelDBIterator(LevelDBStore *store, leveldb::Iterator *it,
                                 const KeyRange &range)
    : store_(store), it_(it), range_(range), status_(Status::OK) {
  if (!it_) {
    status_ = Status::InternalError;
    error_msg_ = "LevelDB iterator is null";
    return;
  }
  seek_to_first();
}

LevelDBIterator::~LevelDBIterator() {
  if (store_) {
    store_->unregister_iterator(it_.get());
  }
}

void LevelDBIterator::seek(const Key &key) {
  if (!it_) {
    status_ = Status::InternalError;
    error_msg_ = "Iterator is null";
    return;
  }

  if (range_.direction == ScanDirection::kForward) {
    // 把 key 夹进区间：区间之前 -> 区间第一条（和 MockIterator 一致，
    // 也和 seek_to_first 的行为一致）
    Key target = key;
    if (range_.start && target < *range_.start) {
      target = *range_.start;
    }
    it_->Seek(target);
  } else {
    // LevelDB 原生不支持反向 Seek
    // 先 Seek 到 key 之后，再 Prev
    if (range_.end && key >= *range_.end) {
      // 上界（排他）之后 -> 夹到区间最后一条：反向视角的"第一"
      seek_to_first();
      return;
    }
    it_->Seek(key);
    if (it_->Valid()) {
      // 如果找到的 key > key，需要 Prev 到 <= key
      if (it_->key().ToString() > key) {
        it_->Prev();
      }
    } else {
      // 如果没找到，Seek 到最后一个
      it_->SeekToLast();
    }
  }

  update_status();
}

void LevelDBIterator::seek_to_first() {
  if (!it_) {
    status_ = Status::InternalError;
    error_msg_ = "Iterator is null";
    return;
  }

  // ✅ 考虑 range 边界
  if (range_.direction == ScanDirection::kForward) {
    if (range_.start) {
      // 有 start 边界，从 start 开始
      it_->Seek(*range_.start);
    } else {
      // 没有 start，从头开始
      it_->SeekToFirst();
    }
  } else {
    // 反向扫描：应该从范围的末尾开始
    if (range_.end) {
      // 有 end 边界，定位到 end 之前
      it_->Seek(*range_.end);
      if (it_->Valid() && it_->key().ToString() >= *range_.end) {
        it_->Prev();
      }
    } else {
      // 没有 end，从末尾开始
      it_->SeekToLast();
    }
  }

  update_status();
}

void LevelDBIterator::seek_to_last() {
  if (!it_) {
    status_ = Status::InternalError;
    error_msg_ = "Iterator is null";
    return;
  }

  if (range_.direction == ScanDirection::kForward) {
    // 正向扫描：最后一个应该在 end 之前
    if (range_.end) {
      it_->Seek(*range_.end);
      if (it_->Valid() && it_->key().ToString() >= *range_.end) {
        it_->Prev();
      }
    } else {
      // 没有 end，直接到末尾
      it_->SeekToLast();
    }

  } else {
    // 反向扫描：最后一个应该是 start 或之后
    if (range_.start) {
      it_->Seek(*range_.start);
      // 如果 key > start，需要后退到 <= start
      if (it_->Valid() && it_->key().ToString() > *range_.start) {
        it_->Prev();
      }
    } else {
      // 没有 start，直接到末尾
      it_->SeekToLast();
    }
  }

  update_status();
}

void LevelDBIterator::next() {
  if (!valid())
    return;

  if (range_.direction == ScanDirection::kForward) {
    it_->Next();
  } else {
    it_->Prev();
  }

  update_status();
}

void LevelDBIterator::prev() {
  if (!valid())
    return;

  if (range_.direction == ScanDirection::kForward) {
    it_->Prev();
  } else {
    it_->Next();
  }

  update_status();
}

bool LevelDBIterator::valid() const {

  if (!it_)
    return false;
  if (status_ != Status::OK)
    return false;
  if (!it_->Valid())
    return false;

  // 检查范围
  return in_range();
}

Key LevelDBIterator::key() const {
  if (!valid())
    return "";
  return it_->key().ToString();
}

ByteValue LevelDBIterator::value() const {
  if (!valid())
    return "";
  return it_->value().ToString();
}

KVPair LevelDBIterator::kvpair() const {
  if (!valid())
    return {"", std::nullopt};
  return {it_->key().ToString(), it_->value().ToString()};
}

Status LevelDBIterator::status() const { return status_; }

std::string LevelDBIterator::error_message() const {
  if (it_ && !it_->status().ok()) {
    return it_->status().ToString();
  }
  return error_msg_;
}

void LevelDBIterator::update_status() {
  if (!it_) {
    status_ = Status::InternalError;
    error_msg_ = "Iterator is null";
    return;
  }

  if (!it_->Valid()) {
    if (it_->status().ok()) {
      status_ = Status::NotFound;
      error_msg_ = "End of iteration";
    } else {
      status_ = Status::IOError;
      error_msg_ = it_->status().ToString();
    }
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

bool LevelDBIterator::in_range() const {
  if (!it_ || !it_->Valid())
    return false;

  const std::string key = it_->key().ToString();
  if (range_.start && key < *range_.start)
    return false;
  if (range_.end && key >= *range_.end)
    return false;

  return true;
}

// ============================================================
// LevelDBStore 实现（存储本体：一个 db_ + 写槽）
// ============================================================

// ----- 生命周期 -----
Status LevelDBStore::open(const DatabaseOptions &options) {
  std::lock_guard<std::mutex> lock(mutex_);

  if (is_open_.load()) {
    return Status::AlreadyExists;
  }

  options_ = options;

  leveldb::Options db_options;
  db_options.create_if_missing = options_.create_if_missing;
  db_options.error_if_exists = options_.error_if_exists;

  // 设置压缩
  if (!options_.compression) {
    db_options.compression = leveldb::kNoCompression;
  }

  // 设置缓存
  if (options_.cache_size_mb > 0) {
    size_t cache_size_bytes = options_.cache_size_mb * 1024 * 1024;
    db_options.block_cache = leveldb::NewLRUCache(cache_size_bytes);
  }

  // 设置过滤器（加速查询）
  db_options.filter_policy = leveldb::NewBloomFilterPolicy(10);

  leveldb::DB *db_ptr = nullptr;
  leveldb::Status status =
      leveldb::DB::Open(db_options, options_.path, &db_ptr);

  if (!status.ok()) {
    std::cerr << "Failed to open LevelDB: " << status.ToString() << std::endl;
    return Status::IOError;
  }

  db_.reset(db_ptr);
  is_open_ = true;

  return Status::OK;
}

Status LevelDBStore::close() {
  std::lock_guard<std::mutex> lock(mutex_);

  if (!is_open_) {
    return Status::NotFound;
  }
  // 检查活跃迭代器
  {
    if (!active_iterators_.empty()) {
      std::cerr << "⚠️  ERROR: Cannot close DB, " << active_iterators_.size()
                << " iterators still active!" << std::endl;
      return Status::Busy; // ✅ 返回错误，让调用者处理
    }
  }
  // 活跃快照同样不许：leveldb 的 Snapshot 钉着旧版本，提前释放会 use-after-free
  if (snapshot_count_ > 0) {
    std::cerr << "⚠️  ERROR: Cannot close DB, " << snapshot_count_
              << " snapshots still active!" << std::endl;
    return Status::Busy;
  }

  is_open_ = false;
  write_owner_ = nullptr; // 存储关了，写槽一并失效

  if (db_) {
    db_.reset();
  }

  return Status::OK;
}

bool LevelDBStore::is_open() const { return is_open_.load(); }

std::shared_ptr<KVEngine> LevelDBStore::connect() {
  return std::make_shared<LevelDBEngine>(shared_from_this());
}

LevelDBStore::~LevelDBStore() {
  if (is_open_.load()) {
    auto s = close();
    if (s != Status::OK) {
      // ✅ 析构时只记录错误，不阻塞
      std::cerr << "⚠️  Warning: DB closed with active iterators" << std::endl;
      // 直接释放 DB，虽然可能导致崩溃，但这是用户的责任
      // 或者选择不释放，让程序退出时 OS 回收
    }
  }
}

// ----- 单条操作（不看事务缓冲：那是连接的事）-----
Status LevelDBStore::get(const Key &key, ByteValue *value,
                         const leveldb::Snapshot *snapshot) const {
  if (!is_open_) {
    return Status::InternalError;
  }

  std::lock_guard<std::mutex> lock(mutex_);

  if (!db_) {
    return Status::InternalError;
  }

  leveldb::ReadOptions options;
  options.verify_checksums = true;
  options.snapshot = snapshot; // 非空 = 读那个版本（事务快照）

  std::string val;
  leveldb::Status status = db_->Get(options, key, &val);

  if (status.IsNotFound()) {
    return Status::NotFound;
  }

  if (!status.ok()) {
    return Status::IOError;
  }

  if (value) {
    *value = val;
  }

  return Status::OK;
}

Status LevelDBStore::put(const Key &key, const ByteValue &value) {
  if (!is_open_) {
    return Status::InternalError;
  }

  std::lock_guard<std::mutex> lock(mutex_);

  if (!db_) {
    return Status::InternalError;
  }

  leveldb::WriteOptions options;
  options.sync = false; // 性能优先

  leveldb::Status status = db_->Put(options, key, value);

  if (!status.ok()) {
    return Status::IOError;
  }

  return Status::OK;
}

Status LevelDBStore::remove(const Key &key) {
  if (!is_open_) {
    return Status::InternalError;
  }

  std::lock_guard<std::mutex> lock(mutex_);

  if (!db_) {
    return Status::InternalError;
  }

  // 先查一遍：leveldb 的 Delete 对不存在的 key 也返回 OK，那样两个引擎的
  // "删不存在的 key"行为就不一致了（Mock 返回 NotFound）。多一次 Get，
  // 换来的是上层看到同一套语义（与 Mock 对齐；事务里的删除仍是幂等的）。
  {
    leveldb::ReadOptions read_options;
    std::string existing;
    const leveldb::Status found = db_->Get(read_options, key, &existing);
    if (found.IsNotFound()) {
      return Status::NotFound;
    }
    if (!found.ok()) {
      return Status::IOError;
    }
  }

  leveldb::WriteOptions options;
  options.sync = false;

  leveldb::Status status = db_->Delete(options, key);

  if (status.IsNotFound()) {
    return Status::NotFound;
  }

  if (!status.ok()) {
    return Status::IOError;
  }

  return Status::OK;
}

bool LevelDBStore::exists(const Key &key,
                          const leveldb::Snapshot *snapshot) const {
  if (!is_open_) {
    return false;
  }

  std::lock_guard<std::mutex> lock(mutex_);

  if (!db_) {
    return false;
  }

  leveldb::ReadOptions options;
  options.snapshot = snapshot;
  std::string value;
  leveldb::Status status = db_->Get(options, key, &value);

  return status.ok();
}

// ----- 批量操作 -----
Status LevelDBStore::get_batch(const std::vector<Key> &keys,
                               MissingKeyPolicy policy,
                               std::vector<std::optional<ByteValue>> *values,
                               const leveldb::Snapshot *snapshot) const {
  if (!is_open_) {
    return Status::InternalError;
  }

  if (!values) {
    return Status::InvalidArgument;
  }

  std::lock_guard<std::mutex> lock(mutex_);

  if (!db_) {
    return Status::InternalError;
  }

  values->clear();
  values->reserve(keys.size());

  leveldb::ReadOptions options;
  options.verify_checksums = true;
  options.snapshot = snapshot;

  for (const auto &key : keys) {
    std::string val;
    leveldb::Status status = db_->Get(options, key, &val);

    if (status.IsNotFound()) {
      if (policy == MissingKeyPolicy::kReturnError) {
        return Status::NotFound;
      }
      values->push_back(std::nullopt);
    } else if (!status.ok()) {
      return Status::IOError;
    } else {
      values->push_back(val);
    }
  }

  return Status::OK;
}

Status LevelDBStore::write_batch(const WriteBatch &batch) {
  if (!is_open_) {
    return Status::InternalError;
  }

  if (batch.empty()) {
    return Status::OK;
  }

  std::lock_guard<std::mutex> lock(mutex_);

  if (!db_) {
    return Status::InternalError;
  }

  return apply_batch_locked(batch);
}

// 直接落到 leveldb。调用方必须已持有 mutex_ ——
// 注意别在持锁的情况下再调 write_batch()，那是同锁重入（死锁）。
Status LevelDBStore::apply_batch_locked(const WriteBatch &batch) {
  if (!db_) {
    return Status::InternalError;
  }

  leveldb::WriteBatch leveldb_batch;

  // 两趟：先整体校验，再整体应用 —— "任何一条 op 非法"时一条都不落
  // （和 Mock 引擎的行为对齐；旧实现会让反区间静默变成空操作）
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
      leveldb_batch.Put(op.data.key, op.data.value.value());
    } else if (op.type == WriteBatch::OpType::kRemoveRange) {
      // [begin, end) 整段删除。当前 leveldb 版本没有 DeleteRange，
      // 这里展开成逐键删除 —— 仍然在**同一个 batch** 里，所以原子性不变；
      // 代价是内存与键数成正比（和不用事务时逐键删是一样的）。
      leveldb::ReadOptions read_options;
      std::unique_ptr<leveldb::Iterator> it(db_->NewIterator(read_options));
      for (it->Seek(op.data.key);
           it->Valid() && it->key().ToString() < op.range_end; it->Next()) {
        leveldb_batch.Delete(it->key());
      }
    } else { // kRemove
      leveldb_batch.Delete(op.data.key);
    }
  }

  leveldb::WriteOptions options;
  options.sync = batch.sync(); // 事务提交会置 true（保证落盘）

  leveldb::Status status = db_->Write(options, &leveldb_batch);

  if (!status.ok()) {
    return Status::IOError;
  }

  return Status::OK;
}

// ----- 迭代器（底座：leveldb 的迭代器创建时就钉住当时的版本）-----
std::unique_ptr<Iterator>
LevelDBStore::new_iterator(const KeyRange &range,
                           const leveldb::Snapshot *snapshot) {
  if (!is_open_) {
    return nullptr;
  }

  std::lock_guard<std::mutex> lock(mutex_);

  if (!db_) {
    return nullptr;
  }

  leveldb::ReadOptions options;
  options.verify_checksums = true;
  options.snapshot = snapshot; // 非空 = 迭代那个版本（事务快照）
  leveldb::Iterator *it = db_->NewIterator(options);
  this->active_iterators_.insert(it);
  return std::make_unique<LevelDBIterator>(this, it, range);
}

std::unique_ptr<Iterator>
LevelDBStore::new_iterator(const KeyRange &range) {
  return new_iterator(range, nullptr);
}

// ----- 快照（事务的一致读视图）-----
const leveldb::Snapshot *LevelDBStore::acquire_snapshot() {
  std::lock_guard<std::mutex> lock(mutex_);
  if (!is_open_ || !db_) {
    return nullptr;
  }
  const leveldb::Snapshot *snapshot = db_->GetSnapshot();
  if (snapshot != nullptr) {
    ++snapshot_count_;
  }
  return snapshot;
}

void LevelDBStore::release_snapshot(const leveldb::Snapshot *snapshot) {
  if (snapshot == nullptr) {
    return;
  }
  std::lock_guard<std::mutex> lock(mutex_);
  if (db_) {
    db_->ReleaseSnapshot(snapshot);
  }
  if (snapshot_count_ > 0) {
    --snapshot_count_;
  }
}

// ----- 写槽（悲观单写者）-----
Status LevelDBStore::acquire_write_slot(const void *owner) {
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

Status LevelDBStore::release_write_slot(const void *owner) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (write_owner_ != owner) {
    return Status::NotFound; // 不是这条连接持有的（或根本没人持有）
  }
  write_owner_ = nullptr;
  return Status::OK;
}

bool LevelDBStore::write_slot_held() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return write_owner_ != nullptr;
}

void LevelDBStore::unregister_iterator(leveldb::Iterator *it) {
  std::lock_guard<std::mutex> lock(mutex_);
  auto iter = active_iterators_.find(it);
  if (iter != active_iterators_.end()) {
    active_iterators_.erase(iter);
  }
}

bool LevelDBStore::has_active_iterators() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return !active_iterators_.empty();
}

// ----- 管理 -----
void LevelDBStore::flush() {
  if (!is_open_ || !db_) {
    return;
  }

  // LevelDB 自动管理 flush，但可以手动 compact
  // 这里不做强制 flush，因为 leveldb 的 Write 已经是持久化的
}

std::string LevelDBStore::stats() const {
  std::lock_guard<std::mutex> lock(mutex_);

  std::ostringstream oss;
  oss << "LevelDBStore Stats:\n";
  oss << "  is_open: " << (is_open_ ? "true" : "false") << "\n";
  oss << "  path: " << options_.path << "\n";
  oss << "  compression: " << (options_.compression ? "enabled" : "disabled")
      << "\n";
  oss << "  cache_size_mb: " << options_.cache_size_mb << "\n";

  if (db_) {
    // 获取 LevelDB 属性
    std::string property;
    if (db_->GetProperty("leveldb.stats", &property)) {
      oss << "\nLevelDB Properties:\n" << property;
    }
  }

  return oss.str();
}

// ============================================================
// LevelDBEngine：一条连接（= 一个 session 的存储视角）
// ============================================================
Status LevelDBEngine::get(const Key &key, ByteValue *value) {
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
  return store_->get(key, value, snapshot_); // 快照非空 = 一致读视图
}

Status LevelDBEngine::put(const Key &key, const ByteValue &value) {
  if (!store_->is_open()) {
    return Status::InternalError;
  }
  if (tx_ != nullptr) {
    const Status slot = ensure_write_slot();
    if (slot != Status::OK) {
      return slot; // 别人在写 -> Busy（悲观单写者）
    }
    tx_->put(key, value);
    return Status::OK;
  }
  // 没有显式事务 = 自动提交写：短暂占用写槽（单写者规则在 Store 上）
  const Status acquired = store_->acquire_write_slot(this);
  if (acquired != Status::OK) {
    return acquired;
  }
  const Status status = store_->put(key, value);
  store_->release_write_slot(this);
  return status;
}

Status LevelDBEngine::remove(const Key &key) {
  if (!store_->is_open()) {
    return Status::InternalError;
  }
  if (tx_ != nullptr) {
    const Status slot = ensure_write_slot();
    if (slot != Status::OK) {
      return slot;
    }
    tx_->remove(key);
    return Status::OK;
  }
  const Status acquired = store_->acquire_write_slot(this);
  if (acquired != Status::OK) {
    return acquired;
  }
  const Status status = store_->remove(key);
  store_->release_write_slot(this);
  return status;
}

bool LevelDBEngine::exists(const Key &key) {
  if (!store_->is_open()) {
    return false;
  }
  if (tx_ != nullptr) {
    const OverlayOp op = tx_->lookup(key);
    if (op.covered()) {
      return op.has_value();
    }
  }
  return store_->exists(key, snapshot_);
}

Status LevelDBEngine::get_batch(const std::vector<Key> &keys,
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
    return store_->get_batch(keys, policy, values, snapshot_);
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

Status LevelDBEngine::write_batch(const WriteBatch &batch) {
  if (!store_->is_open()) {
    return Status::InternalError;
  }
  if (batch.empty()) {
    return Status::OK;
  }

  // 事务里再写批量：路由进缓冲（事务内保持顺序与原子性）
  if (tx_ == nullptr) {
    const Status acquired = store_->acquire_write_slot(this);
    if (acquired != Status::OK) {
      return acquired;
    }
    const Status status = store_->write_batch(batch);
    store_->release_write_slot(this);
    return status;
  }
  const Status slot = ensure_write_slot();
  if (slot != Status::OK) {
    return slot;
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

std::unique_ptr<Iterator> LevelDBEngine::new_iterator(const KeyRange &range) {
  if (!store_->is_open()) {
    return nullptr;
  }
  // leveldb 迭代器：带快照 = 事务的一致读视图；不带 = 创建那一刻的版本
  auto inner = store_->new_iterator(range, snapshot_);
  if (inner == nullptr) {
    return nullptr;
  }
  if (tx_ == nullptr) {
    return inner;
  }
  // 事务里扫描：过滤掉本事务删掉的 key、覆盖本事务改过的值
  return std::make_unique<MergingIterator>(std::move(inner), tx_.get(), range);
}

// ----- 事务（悲观单写者：写槽在 Store 上）-----
Status LevelDBEngine::begin_transaction() {
  if (!store_->is_open()) {
    return Status::InternalError;
  }
  if (tx_ != nullptr) {
    return Status::Busy; // 同一条连接重复 begin
  }
  // 开事务 = 取快照（一致读视图）；写槽等第一次写的时候再抢
  snapshot_ = store_->acquire_snapshot();
  if (snapshot_ == nullptr) {
    return Status::InternalError;
  }
  tx_ = std::make_unique<TxBuffer>();
  return Status::OK;
}

Status LevelDBEngine::acquire_write_slot() {
  if (write_slot_) {
    return Status::OK; // 本连接已经持有
  }
  if (!store_->is_open()) {
    return Status::InternalError;
  }
  const Status acquired = store_->acquire_write_slot(this);
  if (acquired != Status::OK) {
    return acquired; // 另一条连接正在写 -> Busy（悲观单写者）
  }
  write_slot_ = true;
  // 拿到写槽就放掉快照：写槽保证没有别人能提交，"最新已提交 + 自己的缓冲"
  // 本身就是冻结视图（可重复读仍然成立）；而写路径的存在性检查必须看最新
  // 状态，否则会把别人刚提交的同一个主键静默覆盖。
  if (snapshot_ != nullptr) {
    store_->release_snapshot(snapshot_);
    snapshot_ = nullptr;
  }
  return Status::OK;
}

Status LevelDBEngine::ensure_write_slot() {
  return write_slot_ ? Status::OK : acquire_write_slot();
}

void LevelDBEngine::release_write_slot() {
  if (!write_slot_) {
    return;
  }
  store_->release_write_slot(this);
  write_slot_ = false;
}

Status LevelDBEngine::commit_transaction() {
  if (!store_->is_open()) {
    return Status::InternalError;
  }
  if (tx_ == nullptr) {
    return Status::NotFound;
  }
  if (tx_->exceeds()) {
    return Status::InvalidArgument; // 事务太大（调用方应 rollback）
  }
  // 摘下缓冲：apply 阶段不能再走"进缓冲"那条路
  std::unique_ptr<TxBuffer> tx = std::move(tx_);
  WriteBatch batch = tx->to_batch();
  batch.set_sync(true); // 提交必须落盘
  const Status status = store_->write_batch(batch);
  if (status != Status::OK) {
    tx_ = std::move(tx); // 失败：缓冲留着，调用方可以重试或回滚
    return status;       // 写槽也还握着（事务没结束）
  }
  tx_.reset();
  if (snapshot_ != nullptr) {
    store_->release_snapshot(snapshot_);
    snapshot_ = nullptr;
  }
  if (write_slot_) {
    store_->release_write_slot(this);
    write_slot_ = false;
  }
  return Status::OK;
}

Status LevelDBEngine::rollback_transaction() {
  if (!store_->is_open()) {
    return Status::InternalError;
  }
  if (tx_ == nullptr) {
    return Status::NotFound;
  }
  tx_.reset(); // DB 从没被动过：这就是完整的回滚
  if (snapshot_ != nullptr) {
    store_->release_snapshot(snapshot_);
    snapshot_ = nullptr;
  }
  if (write_slot_) {
    store_->release_write_slot(this);
    write_slot_ = false;
  }
  return Status::OK;
}

} // namespace kv
