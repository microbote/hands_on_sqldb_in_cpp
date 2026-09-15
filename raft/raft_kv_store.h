#pragma once

#include <atomic>
#include <map>
#include <memory>
#include <optional>
#include <string>

#include "raft/raft_executor.h"
#include "storage/kv_engine/kv_engine.h"
#include "storage/kv_engine/tx_buffer.h"

namespace raft {

class RaftKVEngine;

// Single-group P1 adapter. SQL still sees kv::KVStore, while writes are
// proposed to Raft and reads are served from the locally applied state machine.
class RaftKVStore final : public kv::KVStore,
                          public std::enable_shared_from_this<RaftKVStore> {
public:
  // `client_endpoints` maps node id -> client-facing "host:port": when this
  // node is not the group leader, leader_hint() reports where the client
  // should reconnect. Empty map = no redirect information.
  RaftKVStore(std::shared_ptr<kv::KVStore> local, RaftExecutor &executor,
              std::map<uint64_t, std::string> client_endpoints = {});
  ~RaftKVStore() override;

  RaftKVStore(const RaftKVStore &) = delete;
  RaftKVStore &operator=(const RaftKVStore &) = delete;

  kv::Status open(const kv::DatabaseOptions &options) override;
  kv::Status close() override;
  bool is_open() const override;
  std::shared_ptr<kv::KVEngine> connect() override;

  std::string name() const override { return "RaftKVStore"; }
  void flush() override;
  std::string stats() const override;
  bool write_slot_held() const override;
  std::optional<kv::LeaderHint> leader_hint() override;

  // Raw KVStore operations intentionally do not bypass Raft. Clients use
  // connect(); the replicated state machine uses its separate local store.
  kv::Status write_batch(const kv::WriteBatch &batch) override;
  std::unique_ptr<kv::Iterator>
  new_iterator(const kv::KeyRange &range) override;

private:
  friend class RaftKVEngine;

  bool acquire_write_slot(const void *owner);
  void release_write_slot(const void *owner);

  std::shared_ptr<kv::KVStore> local_;
  RaftExecutor *executor_ = nullptr;
  std::map<uint64_t, std::string> client_endpoints_;
  std::atomic<bool> write_slot_held_{false};
  const void *write_slot_owner_ = nullptr;
};

// One session-facing connection. Explicit writes remain in a TxBuffer and are
// replicated as one atomic WriteBatch at COMMIT.
class RaftKVEngine final : public kv::KVEngine {
public:
  RaftKVEngine(std::shared_ptr<RaftKVStore> store,
               std::shared_ptr<kv::KVEngine> local_engine,
               uint64_t client_id, RaftExecutor &executor);
  ~RaftKVEngine() override;

  std::shared_ptr<kv::KVStore> store() const override;
  bool is_open() const override;
  kv::Status last_error() const override { return last_error_; }

  kv::Status get(const kv::Key &key, kv::ByteValue *value) override;
  kv::Status put(const kv::Key &key, const kv::ByteValue &value) override;
  kv::Status remove(const kv::Key &key) override;
  bool exists(const kv::Key &key) override;

  kv::Status get_batch(const std::vector<kv::Key> &keys,
                       kv::MissingKeyPolicy policy,
                       std::vector<std::optional<kv::ByteValue>> *values)
      override;
  kv::Status write_batch(const kv::WriteBatch &batch) override;

  std::unique_ptr<kv::Iterator>
  new_iterator(const kv::KeyRange &range) override;

  kv::Status begin_transaction() override;
  kv::Status commit_transaction() override;
  kv::Status rollback_transaction() override;
  bool in_transaction() const override;
  kv::Status acquire_write_slot() override;
  void release_write_slot() override;
  bool has_write_slot() const override;
  bool has_snapshot() const override;

  void flush() override;
  std::string stats() const override;
  std::string name() const override;

private:
  kv::Status ensure_readable();
  kv::Status read_key(const kv::Key &key, kv::ByteValue *value);
  kv::Status propose_batch(const kv::WriteBatch &batch);
  void release_local_snapshot();
  std::unique_ptr<kv::Iterator> make_iterator(const kv::KeyRange &range);

  std::shared_ptr<RaftKVStore> store_;
  std::shared_ptr<kv::KVEngine> local_engine_;
  RaftExecutor &executor_;
  kv::Status last_error_ = kv::Status::OK;
  uint64_t client_id_;
  uint64_t next_request_id_ = 1;
  std::unique_ptr<kv::TxBuffer> tx_;
  bool write_slot_ = false;
};

} // namespace raft
