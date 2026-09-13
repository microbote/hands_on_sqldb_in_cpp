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
using ByteValue =
    std::string; // 明确表示为二进制字节序列，避免与 sql::Value 混淆
struct KVPair {
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

inline const char *status_to_string(Status s) {
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
  enum class OpType {
    kPut,
    kRemove,
    kRemoveRange, // [data.key, range_end) 整段删除（DROP TABLE / TRUNCATE 用）
  };

  struct Op {
    OpType type;
    KVPair data;
    Key range_end; // 仅 kRemoveRange 用（data.key 是区间起点）
  };

  void put(const Key &key, const ByteValue &value) {
    ops_.emplace_back(OpType::kPut, KVPair{key, value});
  }
  void remove(const Key &key) {
    ops_.emplace_back(OpType::kRemove, KVPair{key, std::nullopt});
  }
  // 整段删除：LevelDB 侧是 DeleteRange（O(1) 写一条），Mock 侧遍历删除。
  // 顺序敏感：与 put 的交错顺序就是最终结果（所以 TxBuffer 保留 op log）。
  void remove_range(const Key &begin, const Key &end) {
    ops_.emplace_back(OpType::kRemoveRange, KVPair{begin, std::nullopt}, end);
  }

  // 提交事务时置 true：LevelDB 会用 sync=true 写入（保证落盘）。
  // 普通 write_batch（非事务）保持 false，性能优先。
  WriteBatch &set_sync(bool sync) {
    sync_ = sync;
    return *this;
  }
  bool sync() const { return sync_; }
  void put_all(const std::vector<KVPair> &pairs) {
    for (const auto &[key, value] : pairs) {
      if (value.has_value()) {
        put(key, value.value());
      } else {
        fprintf(stderr, "key:[%s]'s value is empty\n", key.c_str());
        return;
      }
    }
  }
  void remove_all(const std::vector<Key> &keys) {
    for (const auto &key : keys) {
      remove(key);
    }
  }

  const std::vector<Op> &ops() const { return ops_; }
  size_t size() const { return ops_.size(); }
  bool empty() const { return ops_.empty(); }
  void clear() { ops_.clear(); }
  const Op &operator[](size_t index) const { return ops_[index]; }

  // ----- 调试 -----
  std::string to_string() const {
    std::string result =
        "WriteBatch(" + std::to_string(ops_.size()) + " ops): ";
    for (const auto &op : ops_) {
      if (op.type == OpType::kPut) {
        result += "PUT " + op.data.key + "=" + op.data.value.value() + "; ";
      } else if (op.type == OpType::kRemoveRange) {
        result += "REMOVE_RANGE [" + op.data.key + ", " + op.range_end + "); ";
      } else {
        result += "REMOVE " + op.data.key + "; ";
      }
    }
    return result;
  }

private:
  std::vector<Op> ops_;
  bool sync_ = false;
};

// ============================================================
// 扫描方向与范围
// ============================================================

enum class ScanDirection {
  kForward, // 正向扫描 (key 升序)
  kReverse  // 反向扫描 (key 降序)
};

struct KeyRange {
  std::optional<Key> start;
  std::optional<Key> end;
  size_t limit = 0;
  ScanDirection direction = ScanDirection::kForward;

  static KeyRange all() { return {}; }
  static KeyRange range(const Key &start, const Key &end, size_t limit = 0,
                        ScanDirection dir = ScanDirection::kForward) {
    return {start, end, limit, dir};
  }

  static KeyRange reverse_range(const Key &start, const Key &end,
                                size_t limit = 0) {
    return {start, end, limit, ScanDirection::kReverse};
  }

  static KeyRange prefix(const Key &prefix, size_t limit = 0,
                         ScanDirection dir = ScanDirection::kForward) {
    return {prefix, prefix + '\xFF', limit, dir};
  }
  static KeyRange from(const Key &start, size_t limit = 0,
                       ScanDirection dir = ScanDirection::kForward) {
    return {start, std::nullopt, limit, dir};
  }
  static KeyRange to(const Key &end, size_t limit = 0,
                     ScanDirection dir = ScanDirection::kForward) {
    return {std::nullopt, end, limit, dir};
  }

  bool is_all() const { return !start && !end; }
  bool has_limit() const { return limit > 0; }
  bool contains(const Key &key) const {
    if (start && key < *start)
      return false;
    if (end && key >= *end)
      return false;
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

  virtual void seek(const Key &key) = 0;
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

  // 遍历当前迭代器：callback 返回 false = **提前停止**（不是错误，也不打印 ——
  // 库代码不打印）。返回 true 就继续下一条。
  void for_each(
      const std::function<bool(const Key &, const ByteValue &)> &callback) {
    while (valid()) {
      if (!callback(key(), value())) {
        return;
      }
      next();
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

  DatabaseOptions &set_path(const char *p) {
    path = p;
    return *this;
  }

  DatabaseOptions &set_path(const std::string &p) {
    path = p;
    return *this;
  }

  DatabaseOptions &set_create_if_missing(bool v) {
    create_if_missing = v;
    return *this;
  }

  DatabaseOptions &set_error_if_exists(bool v) {
    error_if_exists = v;
    return *this;
  }

  DatabaseOptions &set_cache_size(size_t mb) {
    cache_size_mb = mb;
    return *this;
  }

  DatabaseOptions &set_compression(bool v) {
    compression = v;
    return *this;
  }
};

class KVStore;

// ============================================================
// KVEngine：**一条连接**（一个 session 的存储视角）
//
//   - 连接自带事务状态（TxBuffer），事务里的读/写只影响这条连接；
//   - 多条连接共享同一个 KVStore（见下）：一个进程一份存储，一个客户端
//     连接一条 KVEngine —— 搬到服务器时就是这个形状；
//   - 连接的接口是"按连接看数据"：get/put/scan 都会先问自己的事务缓冲，
//     再问存储。
// ============================================================
class KVEngine {
public:
  virtual ~KVEngine() = default;

  // 这条连接挂在哪个存储上（开新连接用 store()->connect()）
  virtual std::shared_ptr<KVStore> store() const = 0;
  virtual bool is_open() const = 0;

  // ----- 单条操作 -----
  virtual Status get(const Key &key, ByteValue *value) = 0;
  virtual Status put(const Key &key, const ByteValue &value) = 0;
  virtual Status remove(const Key &key) = 0;
  virtual bool exists(const Key &key) = 0;

  // ----- 批量操作 -----
  virtual Status get_batch(const std::vector<Key> &keys,
                           MissingKeyPolicy policy,
                           std::vector<std::optional<ByteValue>> *values) = 0;

  virtual Status write_batch(const WriteBatch &batch) = 0;

  // 批量删除
  Status remove_batch(const std::vector<Key> &keys) {
    WriteBatch batch;
    batch.remove_all(keys);
    return write_batch(batch);
  }

  // ----- 扫描 -----
  Status scan(const KeyRange &range, ScanResult *result) {
    if (!result) {
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
  virtual std::unique_ptr<Iterator> new_iterator(const KeyRange &range) = 0;

  std::unique_ptr<Iterator> new_all_iterator() {
    return new_iterator(KeyRange::all());
  }

  std::unique_ptr<Iterator> new_prefix_iterator(const Key &prefix) {
    return new_iterator(KeyRange::prefix(prefix));
  }

  // ----- 事务（悲观单写者）-----
  //
  // 约定（见 storage/kv_engine/tx_buffer.h）：
  //   - begin 之后所有 put/remove/write_batch 都进缓冲，DB 不动；
  //   - commit 把缓冲合成一个 WriteBatch 一次写入（sync=true，保证落盘）；
  //   - rollback 直接丢弃缓冲 —— DB 从没被动过，"与原状态一致"是构造性的；
  //   - 写槽在 **Store** 上（不是连接上）：全进程同一时刻只允许一条连接
  //     持有写事务，第二条 begin 返回 Status::Busy；
  //   - 没有显式事务的 put/remove/write_batch 是"自动提交写"：它短暂占用
  //     写槽（会话的写语句本来就包在事务里），所以单写者规则对引擎级调用
  //     同样成立。
  virtual Status begin_transaction() { return Status::NotSupported; }
  virtual Status commit_transaction() { return Status::NotSupported; }
  virtual Status rollback_transaction() { return Status::NotSupported; }
  virtual bool in_transaction() const { return false; }

  // ----- 管理 -----
  virtual void flush() = 0;
  virtual std::string stats() const { return ""; }
  virtual std::string name() const = 0;
};

// ============================================================
// KVStore：**存储**（进程内一份）
//
//   - 一个 Store 可以开多条连接（connect()），每条连接 = 一个 session；
//   - **写槽**在 Store 上：悲观单写者 —— 同一时刻只允许一条连接持有写事务
//     （第二条 begin 返回 Status::Busy）；
//   - 写事务的记录（工作集/行锁）以后也挂在这一层：它天然是"连接之间"的东西。
// ============================================================
class KVStore {
public:
  virtual ~KVStore() = default;

  // ----- 生命周期 -----
  // 打开存储（已打开返回 AlreadyExists）；close() 之后所有连接失效
  virtual Status open(const DatabaseOptions &options) = 0;
  virtual Status close() = 0;
  virtual bool is_open() const = 0;

  // 开一条新连接：每条连接有自己的事务状态
  virtual std::shared_ptr<KVEngine> connect() = 0;

  // ----- 管理 -----
  virtual std::string name() const = 0;
  virtual void flush() = 0;
  virtual std::string stats() const { return ""; }

  // 写槽是否被某条连接持有（测试/诊断用）
  virtual bool write_slot_held() const = 0;
};

} // namespace kv
