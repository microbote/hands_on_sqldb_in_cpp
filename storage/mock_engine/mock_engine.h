// mock_engine.h
// Mock KV 存储引擎 - 基于 std::map 的内存实现（测试/开发用）
//
// 两层（见 storage/kv_engine/kv_engine.h 的说明）：
//   - MockStore ：存储本体（进程内一份）：data_ + 锁 + 写槽；
//   - MockEngine：**一条连接**（= 一个 session 的存储视角）：自己的事务缓冲。
// 多条连接共享同一个 Store —— 这条路径就是"搬服务器"要走的形状。

#pragma once

#include <atomic>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <vector>

#include "storage/kv_engine/kv_engine.h"
#include "storage/kv_engine/merging_iterator.h"
#include "storage/kv_engine/tx_buffer.h"

namespace kv {

// ============================================================
// Mock 迭代器：**物化**的扫描
//
// 创建时把区间内的数据拷成 vector（那一下在 Store 的锁里），之后不再碰 map。
// 这样：
//   - 扫描期间别的连接提交不影响这条扫描（和 LevelDB"迭代器创建即钉住版本"
//     对齐，两条引擎的语义一致）；
//   - 迭代器本身不持有锁，一次长扫描不会把写者的提交挡在外面。
// ============================================================
class MockIterator : public Iterator {
public:
  MockIterator(std::vector<KVPair> rows, const KeyRange &range);
  ~MockIterator() override = default;

  // ----- 定位 -----
  void seek(const Key &key) override;
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
  bool in_range(size_t index) const;

  std::vector<KVPair> rows_; // 区间内的行（升序），创建时就固定了
  KeyRange range_;
  size_t index_ = 0; // rows_.size() = 走到末尾
  Status status_;
  std::string error_msg_;
};

// ============================================================
// MockStore：存储本体（进程内一份）
// ============================================================
class MockStore : public KVStore,
                  public std::enable_shared_from_this<MockStore> {
public:
  MockStore() = default;
  ~MockStore() override {
    if (is_open_) {
      close();
    }
  }

  // ----- 生命周期 -----
  Status open(const DatabaseOptions &options) override;
  Status close() override;
  bool is_open() const override;

  // 一条新连接（共享这份 data_ 与写槽）
  std::shared_ptr<KVEngine> connect() override;

  // ----- 管理 -----
  std::string name() const override { return "MockStore"; }
  void flush() override {}
  std::string stats() const override;
  bool write_slot_held() const override;

  // ----- 连接用的原语（都不看事务缓冲；锁在内部）-----
  Status get(const Key &key, ByteValue *value) const;
  Status put(const Key &key, const ByteValue &value);
  Status remove(const Key &key);
  bool exists(const Key &key) const;
  Status get_batch(const std::vector<Key> &keys, MissingKeyPolicy policy,
                   std::vector<std::optional<ByteValue>> *values) const;
  Status write_batch(const WriteBatch &batch) override;
  // 扫描底座：把区间物化成一份快照（不持锁的迭代器）
  std::unique_ptr<Iterator> new_iterator(const KeyRange &range) override;

  // ----- 写槽（悲观单写者：同时只允许一条连接持有写事务）-----
  Status acquire_write_slot(const void *owner);
  Status release_write_slot(const void *owner);

  // ----- 测试辅助 -----
  void clear();
  size_t size() const;
  void dump() const;
  // 取一份数据拷贝（连接开事务时当快照用：Mock 用拷贝模拟 leveldb 的版本视图）
  std::map<Key, ByteValue> data_snapshot() const;
  // 故障注入：置 true 后所有批量写入都失败（用来测"提交失败不留痕"）
  // 也用于确认"批量写入是原子的"：失败时一条 op 都不落。
  void set_fail_writes(bool fail) { fail_writes_ = fail; }

private:
  // 直接落到 data_（不经过事务缓冲）；调用方必须已持有 mutex_
  Status apply_batch_locked(const WriteBatch &batch);

  std::map<Key, ByteValue> data_;
  bool fail_writes_ = false; // 测试用：模拟写入失败
  mutable std::mutex mutex_;
  std::atomic<bool> is_open_{false};
  DatabaseOptions options_;
  const void *write_owner_ = nullptr; // 写槽持有者（连接指针）
};

// ============================================================
// MockEngine：一条连接（= 一个 session 的存储视角）
//
//   - 自己的 TxBuffer：事务里的写只进这里，别的连接看不见；
//   - 读先问缓冲、再问 Store（所以"读自己的写"是天然成立的）。
// ============================================================
class MockEngine : public KVEngine {
public:
  // 单连接用法：自己建一个 Store（测试/CLI 的常见入口）
  MockEngine() : store_(std::make_shared<MockStore>()) {}
  explicit MockEngine(std::shared_ptr<MockStore> store)
      : store_(std::move(store)) {}
  // 连接带着未提交的事务析构 = 回滚，并且必须把写槽还回去
  ~MockEngine() override {
    tx_.reset();
    snapshot_.reset();
    if (write_slot_) {
      store_->release_write_slot(this);
    }
  }

  std::shared_ptr<KVStore> store() const override { return store_; }
  bool is_open() const override { return store_->is_open(); }

  // ----- 单条操作 -----
  Status get(const Key &key, ByteValue *value) override;
  Status put(const Key &key, const ByteValue &value) override;
  Status remove(const Key &key) override;
  bool exists(const Key &key) override;

  // ----- 批量操作 -----
  Status get_batch(const std::vector<Key> &keys, MissingKeyPolicy policy,
                   std::vector<std::optional<ByteValue>> *values) override;

  Status write_batch(const WriteBatch &batch) override;

  // ----- 迭代器 -----
  std::unique_ptr<Iterator> new_iterator(const KeyRange &range) override;

  // ----- 事务（悲观单写者：写槽在 Store 上）-----
  Status begin_transaction() override;
  Status commit_transaction() override;
  Status rollback_transaction() override;
  bool in_transaction() const override { return tx_ != nullptr; }
  Status acquire_write_slot() override;
  void release_write_slot() override;
  bool has_write_slot() const override;
  bool has_snapshot() const override { return snapshot_.has_value(); }

  // ----- 管理 -----
  void flush() override { store_->flush(); }
  std::string stats() const override;
  std::string name() const override { return "MockEngine"; }

  // ----- 测试辅助（转发给 Store）-----
  void clear() { store_->clear(); }
  size_t size() const { return store_->size(); }
  void dump() const { store_->dump(); }
  void set_fail_writes(bool fail) { store_->set_fail_writes(fail); }

private:
  // 当前读视图：快照（若在只读事务里）或 Store 的最新数据
  bool in_snapshot() const { return snapshot_.has_value(); }
  // 第一次写之前确保拿到写槽（拿不到 -> Busy）
  Status ensure_write_slot();

  std::shared_ptr<MockStore> store_;
  std::unique_ptr<TxBuffer> tx_; // 本连接的事务缓冲（非空 = 事务进行中）
  // 事务快照：begin 时拷一份（Mock 是测试双，表小；语义与 leveldb 一致）
  std::optional<std::map<Key, ByteValue>> snapshot_;
  bool write_slot_ = false; // 本连接是否持有写槽
};

} // namespace kv
