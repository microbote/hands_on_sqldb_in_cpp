// mock_engine.h
// Mock KV 存储引擎 - 基于 std::map 的内存实现
// 用于测试和开发环境

#pragma once

#include <atomic>
#include <map>
#include <mutex>

#include "storage/kv_engine/kv_engine.h"

namespace kv {

// ============================================================
// Mock 迭代器
// ============================================================
class MockIterator : public Iterator {
 public:
  MockIterator(const std::map<Key, ByteValue>* data, const KeyRange& range);
  ~MockIterator() override = default;

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

  const std::map<Key, ByteValue>* data_;
  KeyRange range_;
  std::map<Key, ByteValue>::const_iterator pos_;
  Status status_;
  std::string error_msg_;
};

// ============================================================
// Mock 存储引擎
// ============================================================
class MockEngine : public KVEngine {
 public:
  MockEngine() = default;
  ~MockEngine() override {
    if (is_open_) {
      close_database();
    }
  }

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


  // ----- 管理 -----
  void flush() override;
  std::string stats() const override;
  std::string name() const override { return "MockEngine"; }

  // ----- 测试辅助 -----
  void clear();
  size_t size() const;
  void dump() const;

 private:
  std::map<Key, ByteValue> data_;
  mutable std::mutex mutex_;
  std::atomic<bool> is_open_{false};
  DatabaseOptions options_;
};

}  // namespace kv