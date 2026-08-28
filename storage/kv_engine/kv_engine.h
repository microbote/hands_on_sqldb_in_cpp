// kv_engine.h
#pragma once

#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace kv {

// ============================================================
// 基础类型定义
// ============================================================

using Key = std::string;
using ByteValue = std::string;  // 明确表示为二进制字节序列，避免与 sql::Value 混淆
struct KVPair{
  Key key;
  std::optional<ByteValue> value;
};

// ============================================================
// 错误码
// ============================================================

enum class Status {
  OK = 0,
  NotFound,
  AlreadyExists,
  InvalidArgument,
  NotSupported,
  IOError,
  InternalError,
  TransactionConflict,
  Busy,
  Timeout,
};

inline const char* status_to_string(Status s) {
  switch (s) {
    case Status::OK:
      return "OK";
    case Status::NotFound:
      return "NotFound";
    case Status::AlreadyExists:
      return "AlreadyExists";
    case Status::InvalidArgument:
      return "InvalidArgument";
    case Status::NotSupported:
      return "NotSupported";
    case Status::IOError:
      return "IOError";
    case Status::InternalError:
      return "InternalError";
    case Status::TransactionConflict:
      return "TransactionConflict";
    case Status::Busy:
      return "Busy";
    case Status::Timeout:
      return "Timeout";
    default:
      return "Unknown";
  }
}

// ============================================================
// 缺失键策略
// ============================================================

enum class MissingKeyPolicy {
  kReturnEmpty,
  kReturnError,
};

// ============================================================
// 写入批次
// ============================================================

class WriteBatch {
 public:
  enum class OpType { kPut, kRemove };

  struct Op {
    OpType type;
    KVPair data;
  };

  void put(const Key& key, const ByteValue& value){
    ops_.emplace_back(OpType::kPut, KVPair{key, value});
  }
  void remove(const Key& key){
    ops_.emplace_back(OpType::kRemove, KVPair{key, std::nullopt});
  }
  void put_all(const std::vector<KVPair>& pairs){
    for (const auto& [key, value] : pairs) {
      if(value.has_value()){
        put(key, value.value());
      }
      else{
        fprintf(stderr, "key:[%s]'s value is empty\n", key.c_str());
        return;
      }
    }
  }
  void remove_all(const std::vector<Key>& keys){
    for (const auto& key : keys) {
      remove(key);
    }
  }

  const std::vector<Op>& ops() const { return ops_; }
  size_t size() const { return ops_.size(); }
  bool empty() const { return ops_.empty(); }
  void clear() { ops_.clear(); }
  const Op& operator[](size_t index) const { return ops_[index]; }

  // ----- 调试 -----
  std::string to_string() const {
    std::string result =
        "WriteBatch(" + std::to_string(ops_.size()) + " ops): ";
    for (const auto& op : ops_) {
      if (op.type == OpType::kPut) {
        result += "PUT " + op.data.key + "=" + op.data.value.value() + "; ";
      } else {
        result += "REMOVE " + op.data.key + "; ";
      }
    }
    return result;
  }

 private:
  std::vector<Op> ops_;
};

// ============================================================
// 扫描方向与范围
// ============================================================

enum class ScanDirection {
  kForward,  // 正向扫描 (key 升序)
  kReverse   // 反向扫描 (key 降序)
};

struct KeyRange {
  std::optional<Key> start;
  std::optional<Key> end;
  size_t limit = 0;
  ScanDirection direction = ScanDirection::kForward;

  static KeyRange all() { return {}; }
  static KeyRange range(const Key& start, const Key& end, size_t limit = 0,
                        ScanDirection dir = ScanDirection::kForward){
                          return {start, end, limit, dir};
                        }

  static KeyRange reverse_range(const Key& start, const Key& end, size_t limit = 0){
    return {start, end, limit, ScanDirection::kReverse};
  }

  static KeyRange prefix(const Key& prefix, size_t limit = 0,
                         ScanDirection dir = ScanDirection::kForward){
                           return {prefix, prefix + '\xFF', limit, dir};
                         }
  static KeyRange from(const Key& start, size_t limit = 0,
                       ScanDirection dir = ScanDirection::kForward){
                         return {start, std::nullopt, limit, dir};
                       }
  static KeyRange to(const Key& end, size_t limit = 0,
                     ScanDirection dir = ScanDirection::kForward){
                       return {std::nullopt, end, limit, dir};
                     }

  bool is_all() const { return !start && !end; }
  bool has_limit() const { return limit > 0; }
  bool contains(const Key& key) const {
    if (start && key < *start) return false;
    if (end && key >= *end) return false;
    return true;
  }

  // ----- 调试 -----
  std::string to_string() const {
    std::string result = "KeyRange[";
    result += start ? "start=" + *start : "start=∞";
    result += ", ";
    result += end ? "end=" + *end : "end=∞";
    result += ", limit=" + std::to_string(limit);
    result += ", dir=" +
              std::string(direction == ScanDirection::kForward ? "fwd" : "rev");
    result += "]";
    return result;
  }
};

// ============================================================
// 扫描结果
// ============================================================

struct ScanResult {
  std::vector<KVPair> pairs;
  std::optional<Key> next_start_key;

  bool has_more() const { return next_start_key.has_value(); }
  bool empty() const { return pairs.empty(); }
  size_t size() const { return pairs.size(); }
};

// ============================================================
// 迭代器
// ============================================================

class Iterator {
 public:
  virtual ~Iterator() = default;

  virtual void seek(const Key& key) = 0;
  virtual void seek_to_first() = 0;
  virtual void seek_to_last() = 0;

  virtual void next() = 0;
  virtual void prev() = 0;

  virtual bool valid() const = 0;
  virtual Key key() const = 0;
  virtual ByteValue value() const = 0;
  virtual KVPair kvpair() const = 0;
  virtual Status status() const = 0;
  virtual std::string error_message() const = 0;

  void for_each(std::function<bool(const Key&, const ByteValue&)> callback,
    bool break_if_error = false) {
    {
      while (valid()) {
        bool ok =callback(key(), value());
        if (!ok) {
          fprintf(stderr, "callback error on Key: %s, Value : %s\n", key().c_str(), value().c_str());
          if(break_if_error) {
            break;
          }
        }
        next();
      }
    }
  }
  std::vector<KVPair> collect(size_t max_count = 0) {
    std::vector<KVPair> result;
    size_t count = 0;
    while (valid()) {
      result.push_back(kvpair());
      count++;
      if (max_count > 0 && count >= max_count) {
        break;
      }
      next();
    }
    return result;
  }
};

// ============================================================
// KV 存储引擎抽象接口
// ============================================================
struct DatabaseOptions {
  std::string path;
  bool create_if_missing = true;
  bool error_if_exists = false;
  size_t cache_size_mb = 0;
  bool compression = true;

  DatabaseOptions& set_path(const char * p) {
    path = p;
    return *this;
  }

  DatabaseOptions& set_path(const std::string& p) {
    path = p;
    return *this;
  }

  DatabaseOptions& set_create_if_missing(bool v) {
    create_if_missing = v;
    return *this;
  }

  DatabaseOptions& set_error_if_exists(bool v) {
    error_if_exists = v;
    return *this;
  }

  DatabaseOptions& set_cache_size(size_t mb) {
    cache_size_mb = mb;
    return *this;
  }

  DatabaseOptions& set_compression(bool v) {
    compression = v;
    return *this;
  }
};

class KVEngine {
 public:
  virtual ~KVEngine() = default;

  // ----- 生命周期 -----
  virtual Status open_database(DatabaseOptions options) = 0;
  virtual Status close_database() = 0;
  virtual bool is_open() const = 0;

  // ----- 单条操作 -----
  virtual Status get(const Key& key, ByteValue* value) = 0;
  virtual Status put(const Key& key, const ByteValue& value) = 0;
  virtual Status remove(const Key& key) = 0;
  virtual bool exists(const Key& key) = 0;

  // ----- 批量操作 -----
  virtual Status get_batch(const std::vector<Key>& keys,
                           MissingKeyPolicy policy,
                           std::vector<std::optional<ByteValue>>* values) = 0;

  virtual Status write_batch(const WriteBatch& batch) = 0;

  // 批量删除
  Status remove_batch(const std::vector<Key>& keys) {
    WriteBatch batch;
    batch.remove_all(keys);
    return write_batch(batch);
  }

  // ----- 扫描 -----
  Status scan(const KeyRange& range, ScanResult* result) {
    if(!result){
      return Status::InvalidArgument;
    }
    // 默认实现：使用迭代器
    auto it = new_iterator(range);
    if (!it) {
      return Status::InternalError;
    }

    size_t count = 0;
    while (it->valid()) {
      result->pairs.push_back(it->kvpair());
      count++;
      if (range.limit > 0 && count >= range.limit) {
        it->next();
        if (it->valid()) {
          result->next_start_key = it->key();
        }
        break;
      }
      it->next();
    }

    if (it->status() != Status::OK && it->status() != Status::NotFound) {
      return it->status();
    }

    return Status::OK;
  }

  // ----- 迭代器 -----
  virtual std::unique_ptr<Iterator> new_iterator(const KeyRange& range) = 0;

  std::unique_ptr<Iterator> new_all_iterator(){
    return new_iterator(KeyRange::all());
  }

  std::unique_ptr<Iterator> new_prefix_iterator(const Key& prefix) {
    return new_iterator(KeyRange::prefix(prefix));
  }

  // ----- 事务 (可选) -----
  virtual Status begin_transaction() { return Status::NotSupported; }
  virtual Status commit_transaction() { return Status::NotSupported; }
  virtual Status rollback_transaction() { return Status::NotSupported; }

  // ----- 管理 -----
  virtual void flush() = 0;
  virtual std::string stats() const { return ""; }
  virtual std::string name() const = 0;

protected:

};



}  // namespace kv