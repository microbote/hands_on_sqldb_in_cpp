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

LevelDBIterator::LevelDBIterator(
  LevelDBEngine* engine,
  leveldb::Iterator* it, const KeyRange& range)
    : engine_(engine), it_(it), range_(range), status_(Status::OK) {
  if (!it_) {
    status_ = Status::InternalError;
    error_msg_ = "LevelDB iterator is null";
    return;
  }
  seek_to_first();

}

LevelDBIterator::~LevelDBIterator() {
  if (engine_) {
    engine_->unregister_iterator(it_.get());
  }
}

void LevelDBIterator::seek(const Key& key) {
  if (!it_) {
    status_ = Status::InternalError;
    error_msg_ = "Iterator is null";
    return;
  }

  if (range_.direction == ScanDirection::kForward) {
    it_->Seek(key);
  } else {
    // LevelDB 原生不支持反向 Seek
    // 先 Seek 到 key 之后，再 Prev
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
  if (!valid()) return;

  if (range_.direction == ScanDirection::kForward) {
    it_->Next();
  } else {
    it_->Prev();
  }

  update_status();
}

void LevelDBIterator::prev() {
  if (!valid()) return;

  if (range_.direction == ScanDirection::kForward) {
    it_->Prev();
  } else {
    it_->Next();
  }

  update_status();
}

bool LevelDBIterator::valid() const {

  if (!it_) return false;
  if (status_ != Status::OK) return false;
  if (!it_->Valid()) return false;

  // 检查范围
  return in_range();
}

Key LevelDBIterator::key() const {
  if (!valid()) return "";
  return it_->key().ToString();
}

ByteValue LevelDBIterator::value() const {
  if (!valid()) return "";
  return it_->value().ToString();
}

KVPair LevelDBIterator::kvpair() const {
  if (!valid()) return {"", std::nullopt};
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
  if (!it_ || !it_->Valid()) return false;

  const std::string key = it_->key().ToString();
  if (range_.start && key < *range_.start) return false;
  if (range_.end && key >= *range_.end) return false;

  return true;
}

// ============================================================
// LevelDBEngine 实现
// ============================================================

// ----- 生命周期 -----
Status LevelDBEngine::open_database(DatabaseOptions options) {
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

  leveldb::DB* db_ptr = nullptr;
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

Status LevelDBEngine::close_database() {
  std::lock_guard<std::mutex> lock(mutex_);

  if (!is_open_) {
    return Status::NotFound;
  }
  // 检查活跃迭代器
  {
    if (!active_iterators_.empty()) {
      std::cerr << "⚠️  ERROR: Cannot close DB, " << active_iterators_.size()
                << " iterators still active!" << std::endl;
      return Status::Busy;  // ✅ 返回错误，让调用者处理
    }
  }

  is_open_ = false;

  if (db_) {
    db_.reset();
  }


  return Status::OK;
}

bool LevelDBEngine::is_open() const { return is_open_.load(); }

LevelDBEngine::~LevelDBEngine() {
  if (is_open_.load()) {
    auto s = close_database();
    if (s != Status::OK) {
      // ✅ 析构时只记录错误，不阻塞
      std::cerr << "⚠️  Warning: DB closed with active iterators" << std::endl;
      // 直接释放 DB，虽然可能导致崩溃，但这是用户的责任
      // 或者选择不释放，让程序退出时 OS 回收
    }
  }
}

// ----- 单条操作 -----
Status LevelDBEngine::get(const Key& key, ByteValue* value) {
  if (!is_open_) {
    return Status::InternalError;
  }

  std::lock_guard<std::mutex> lock(mutex_);

  if (!db_) {
    return Status::InternalError;
  }

  leveldb::ReadOptions options;
  options.verify_checksums = true;

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

Status LevelDBEngine::put(const Key& key, const ByteValue& value) {
  if (!is_open_) {
    return Status::InternalError;
  }

  std::lock_guard<std::mutex> lock(mutex_);

  if (!db_) {
    return Status::InternalError;
  }

  leveldb::WriteOptions options;
  options.sync = false;  // 性能优先

  leveldb::Status status = db_->Put(options, key, value);

  if (!status.ok()) {
    return Status::IOError;
  }

  return Status::OK;
}

Status LevelDBEngine::remove(const Key& key) {
  if (!is_open_) {
    return Status::InternalError;
  }

  std::lock_guard<std::mutex> lock(mutex_);

  if (!db_) {
    return Status::InternalError;
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

bool LevelDBEngine::exists(const Key& key) {
  if (!is_open_) {
    return false;
  }

  std::lock_guard<std::mutex> lock(mutex_);

  if (!db_) {
    return false;
  }

  leveldb::ReadOptions options;
  std::string value;
  leveldb::Status status = db_->Get(options, key, &value);

  return status.ok();
}

// ----- 批量操作 -----
Status LevelDBEngine::get_batch(const std::vector<Key>& keys,
                                MissingKeyPolicy policy,
                                std::vector<std::optional<ByteValue>>* values) {
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

  for (const auto& key : keys) {
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

Status LevelDBEngine::write_batch(const WriteBatch& batch) {
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

  leveldb::WriteBatch leveldb_batch;

  for (const auto& op : batch.ops()) {
    if (op.type == WriteBatch::OpType::kPut) {
      if (op.data.value.has_value()) {
        leveldb_batch.Put(op.data.key, op.data.value.value());
      } else {
        // value 为空，视为删除
        leveldb_batch.Delete(op.data.key);
      }
    } else {  // kRemove
      leveldb_batch.Delete(op.data.key);
    }
  }

  leveldb::WriteOptions options;
  options.sync = false;

  leveldb::Status status = db_->Write(options, &leveldb_batch);

  if (!status.ok()) {
    return Status::IOError;
  }

  return Status::OK;
}

// ----- 迭代器 -----
std::unique_ptr<Iterator> LevelDBEngine::new_iterator(const KeyRange& range) {
  if (!is_open_) {
    return nullptr;
  }

  std::lock_guard<std::mutex> lock(mutex_);

  if (!db_) {
    return nullptr;
  }

  leveldb::ReadOptions options;
  options.verify_checksums = true;
  leveldb::Iterator* it = db_->NewIterator(options);
  this->active_iterators_.insert(it);
  auto iter = std::make_unique<LevelDBIterator>(this, it, range);
  return iter;
}

void LevelDBEngine::unregister_iterator(leveldb::Iterator* it) {
  std::lock_guard<std::mutex> lock(mutex_);
  auto iter = active_iterators_.find(it);
  if (iter != active_iterators_.end()) {
    active_iterators_.erase(iter);
  }
}

bool LevelDBEngine::has_active_iterators() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return !active_iterators_.empty();
}


// ----- 管理 -----
void LevelDBEngine::flush() {
  if (!is_open_ || !db_) {
    return;
  }

  // LevelDB 自动管理 flush，但可以手动 compact
  // 这里不做强制 flush，因为 leveldb 的 Write 已经是持久化的
}

std::string LevelDBEngine::stats() const {
  std::lock_guard<std::mutex> lock(mutex_);

  std::ostringstream oss;
  oss << "LevelDBEngine Stats:\n";
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

}  // namespace kv