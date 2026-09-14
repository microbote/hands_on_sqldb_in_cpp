// leveldb_engine.h
#pragma once

#include <leveldb/db.h>
#include <leveldb/iterator.h>
#include <leveldb/options.h>
#include <leveldb/write_batch.h>

#include <atomic>
#include <memory>
#include <mutex>
#include <unordered_set>
#include <vector>

#include "storage/kv_engine/kv_engine.h"
#include "storage/kv_engine/merging_iterator.h"
#include "storage/kv_engine/tx_buffer.h"

namespace kv {

// ============================================================
// LevelDB 迭代器
// ============================================================
class LevelDBEngine;
class LevelDBStore;

class LevelDBIterator : public Iterator {
public:
  explicit LevelDBIterator(LevelDBStore *store, leveldb::Iterator *it,
                           const KeyRange &range);
  ~LevelDBIterator() override;

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
  bool in_range() const;

  LevelDBStore *store_; // 弱引用，不持有所有权（析构时注销迭代器）
  std::unique_ptr<leveldb::Iterator> it_;
  KeyRange range_;
  Status status_;
  std::string error_msg_;
};

// ============================================================
// LevelDBStore：存储本体（进程内一份）
//
//   - 持有唯一的 leveldb::DB 句柄 —— 两条连接必须是同一个 DB 对象，
//     不能对同一个目录开两次（leveldb 自己的目录锁挡不住同进程的第二次
//     打开，那样会真的写坏数据）；
//   - 写槽（悲观单写者）也在这里：同一时刻只允许一条连接持有写事务。
// ============================================================
class LevelDBStore : public KVStore,
                     public std::enable_shared_from_this<LevelDBStore> {
public:
  LevelDBStore() = default;
  ~LevelDBStore() override;

  // ----- 生命周期 -----
  Status open(const DatabaseOptions &options) override;
  Status close() override;
  bool is_open() const override;

  // 一条新连接（共享这个 db_ 与写槽）
  std::shared_ptr<KVEngine> connect() override;

  // ----- 管理 -----
  std::string name() const override { return "LevelDBStore"; }
  void flush() override;
  std::string stats() const override;
  bool write_slot_held() const override;

  // ----- 连接用的原语（都不看事务缓冲；锁在内部）-----
  // snapshot 非空 = 读那个版本（事务的一致读视图）
  Status get(const Key &key, ByteValue *value,
             const leveldb::Snapshot *snapshot = nullptr) const;
  Status put(const Key &key, const ByteValue &value);
  Status remove(const Key &key);
  bool exists(const Key &key,
              const leveldb::Snapshot *snapshot = nullptr) const;
  Status get_batch(const std::vector<Key> &keys, MissingKeyPolicy policy,
                   std::vector<std::optional<ByteValue>> *values,
                   const leveldb::Snapshot *snapshot = nullptr) const;
  Status write_batch(const WriteBatch &batch) override;
  std::unique_ptr<Iterator> new_iterator(const KeyRange &range) override;
  std::unique_ptr<Iterator>
  new_iterator(const KeyRange &range,
               const leveldb::Snapshot *snapshot = nullptr);

  // ----- 写槽（悲观单写者）-----
  Status acquire_write_slot(const void *owner);
  Status release_write_slot(const void *owner);

  // ----- 快照（事务的一致读视图）-----
  // leveldb 的 Snapshot 会钉住旧版本（阻止 compaction 回收）——必须在事务
  // 结束/连接析构时释放；close() 时若还有活跃快照会返回 Busy。
  const leveldb::Snapshot *acquire_snapshot();
  void release_snapshot(const leveldb::Snapshot *snapshot);

  // 活跃迭代器的登记/注销（close 时不允许还有活跃迭代器）
  void unregister_iterator(leveldb::Iterator *it);
  bool has_active_iterators() const;

private:
  // 直接落到 leveldb；调用方必须已持有 mutex_
  Status apply_batch_locked(const WriteBatch &batch);

  std::unique_ptr<leveldb::DB> db_;
  DatabaseOptions options_;
  mutable std::mutex mutex_;
  std::atomic<bool> is_open_{false};
  const void *write_owner_ = nullptr; // 写槽持有者（连接指针）
  size_t snapshot_count_ = 0;         // 活跃快照数（close 时要为 0）
  std::unordered_set<leveldb::Iterator *> active_iterators_;
};

// ============================================================
// LevelDBEngine：一条连接（= 一个 session 的存储视角）
// ============================================================
class LevelDBEngine : public KVEngine {
public:
  explicit LevelDBEngine(std::shared_ptr<LevelDBStore> store)
      : store_(std::move(store)) {}
  // 连接带着未提交的事务析构 = 回滚，并且必须把写槽还回去
  ~LevelDBEngine() override {
    tx_.reset();
    if (snapshot_ != nullptr) {
      store_->release_snapshot(snapshot_);
      snapshot_ = nullptr;
    }
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
  bool has_write_slot() const override { return write_slot_; }
  bool has_snapshot() const override { return snapshot_ != nullptr; }

  // ----- 管理 -----
  void flush() override { store_->flush(); }
  std::string stats() const override { return store_->stats(); }
  std::string name() const override { return "LevelDBEngine"; }

private:
  // 第一次写之前确保拿到写槽（拿不到 -> Busy）；拿到写槽会放掉快照
  Status ensure_write_slot();

  std::shared_ptr<LevelDBStore> store_;
  std::unique_ptr<TxBuffer> tx_; // 本连接的事务缓冲（非空 = 事务进行中）
  const leveldb::Snapshot *snapshot_ = nullptr; // 事务的一致读视图
  bool write_slot_ = false;                     // 本连接是否持有写槽
};

} // namespace kv
