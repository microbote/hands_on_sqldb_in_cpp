// leveldb_engine.h
#pragma once

#include <leveldb/db.h>
#include <leveldb/iterator.h>
#include <leveldb/options.h>
#include <leveldb/write_batch.h>

#include <atomic>
#include <memory>
#include <mutex>
#include <vector>
#include <unordered_set>

#include "storage/kv_engine/kv_engine.h"

namespace kv {

// ============================================================
// LevelDB 迭代器
// ============================================================
class LevelDBEngine;

class LevelDBIterator : public Iterator {
 public:
  explicit LevelDBIterator(LevelDBEngine* engine,
                           leveldb::Iterator* it, const KeyRange& range);
  ~LevelDBIterator() override;

  // ----- 定位 -----
  void seek(const Key& key) override;
  void seek_to_first() override;
  void seek_to_last() override;

  // ----- 移动 -----
  void next() override;
  void prev() override;

  // ----- 状态 -----
  bool valid() const override;
  Key key() const override;
  ByteValue value() const override;
  KVPair kvpair() const override;
  Status status() const override;
  std::string error_message() const override;

 private:
  void update_status();
  bool in_range() const;

  LevelDBEngine* engine_; // 弱引用，不持有所有权
  std::unique_ptr<leveldb::Iterator> it_;
  KeyRange range_;
  Status status_;
  std::string error_msg_;
};

// ============================================================
// LevelDB 存储引擎
// ============================================================
class LevelDBEngine : public KVEngine,
                      public std::enable_shared_from_this<LevelDBEngine> {
 public:
  LevelDBEngine() = default;
  ~LevelDBEngine() override ;

  // 将父类的 new_iterator 重载引入
  using KVEngine::new_iterator;

  // ----- 生命周期 -----
  Status open_database(DatabaseOptions options) override;
  Status close_database() override;
  bool is_open() const override;

  // ----- 单条操作 -----
  Status get(const Key& key, ByteValue* value) override;
  Status put(const Key& key, const ByteValue& value) override;
  Status remove(const Key& key) override;
  bool exists(const Key& key) override;

  // ----- 批量操作 -----
  Status get_batch(const std::vector<Key>& keys, MissingKeyPolicy policy,
                   std::vector<std::optional<ByteValue>>* values) override;

  Status write_batch(const WriteBatch& batch) override;

  // ----- 迭代器 -----
  std::unique_ptr<Iterator> new_iterator(const KeyRange& range) override;
  void unregister_iterator(leveldb::Iterator* it);
  bool has_active_iterators() const;

  // ----- 管理 -----
  void flush() override;
  std::string stats() const override;
  std::string name() const override { return "LevelDBEngine"; }

 protected:


 private:
  // 将 kv::KeyRange 转换为 leveldb 的 KeyRange（用于实际扫描）
  void apply_range_bounds(leveldb::Iterator* it, const KeyRange& range);

  std::unique_ptr<leveldb::DB> db_;
  DatabaseOptions options_;
  mutable std::mutex mutex_;
  std::atomic<bool> is_open_{false};

  std::unordered_set<leveldb::Iterator*> active_iterators_;
};

}  // namespace kv