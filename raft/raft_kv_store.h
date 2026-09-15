#pragma once

#include <atomic>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <string>

#include "raft/group_router.h"
#include "raft/raft_executor.h"
#include "storage/kv_engine/kv_engine.h"
#include "storage/kv_engine/tx_buffer.h"

namespace raft {

class RaftKVEngine;

// P2 adapter (slice 1). SQL still sees kv::KVStore, while writes are proposed
// to the owning raft group and reads are served from the locally applied state
// machine. Keys are routed through GroupRouter: the "@system/" prefix lives in
// group 0 and data ranges map to their own groups. A single-group deployment
// (no data ranges) behaves exactly like the pre-multi-group adapter.
class RaftKVStore final : public kv::KVStore,
                          public std::enable_shared_from_this<RaftKVStore> {
public:
  // Single-group convenience: one executor owns the whole key space.
  // `client_endpoints` maps node id -> client-facing "host:port": when this
  // node is not the group leader, leader_hint() reports where the client
  // should reconnect. Empty map = no redirect information.
  RaftKVStore(std::shared_ptr<kv::KVStore> local, RaftExecutor &executor,
              std::map<uint64_t, std::string> client_endpoints = {});

  // Multi-group: executors[group_id] is the raft executor of that group and
  // must cover every group the router can return (at least group 0).
  RaftKVStore(std::shared_ptr<kv::KVStore> local,
              std::map<uint64_t, RaftExecutor *> executors, GroupRouter router,
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

  const GroupRouter &router() const { return router_; }
  RaftExecutor &executor_for(uint64_t group) const;
  const std::map<uint64_t, RaftExecutor *> &executors() const {
    return executors_;
  }

private:
  friend class RaftKVEngine;

  bool acquire_write_slot(uint64_t group, const void *owner);
  void release_write_slot(uint64_t group, const void *owner);

  std::shared_ptr<kv::KVStore> local_;
  std::map<uint64_t, RaftExecutor *> executors_;
  GroupRouter router_;
  std::map<uint64_t, std::string> client_endpoints_;
  struct WriteSlot {
    bool held = false;
    const void *owner = nullptr;
  };
  std::map<uint64_t, WriteSlot> write_slots_;
};

// One session-facing connection. Explicit writes remain in a TxBuffer and are
// replicated as one atomic WriteBatch at COMMIT.
class RaftKVEngine final : public kv::KVEngine {
public:
  RaftKVEngine(std::shared_ptr<RaftKVStore> store,
               std::shared_ptr<kv::KVEngine> local_engine, uint64_t client_id);
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

  // P2 slice: strict (default) rejects a transaction that reads a second
  // group; loose allows it and freezes the transaction as read-only. The
  // CLIENT_OPTIONS frame that carries this from the client is the next slice.
  void set_loose_cross_group_reads(bool loose) {
    loose_cross_group_reads_ = loose;
  }

private:
  kv::Status ensure_readable(uint64_t group);
  // Cross-group transaction rules (DESIGN §3.1): bind on the first data touch,
  // writes never cross groups, and a crossed (read-only) transaction can never
  // write.
  kv::Status check_group(uint64_t group, bool is_write);
  kv::Status read_key(const kv::Key &key, kv::ByteValue *value);
  kv::Status propose_batch(uint64_t group, const kv::WriteBatch &batch);
  kv::Status acquire_write_slot_for(uint64_t group);
  void release_local_snapshot();
  std::unique_ptr<kv::Iterator> make_iterator(const kv::KeyRange &range);

  std::shared_ptr<RaftKVStore> store_;
  std::shared_ptr<kv::KVEngine> local_engine_;
  kv::Status last_error_ = kv::Status::OK;
  uint64_t client_id_;
  uint64_t next_request_id_ = 1;
  std::unique_ptr<kv::TxBuffer> tx_;
  bool write_slot_ = false;
  std::optional<uint64_t> write_group_;
  // Transaction bookkeeping (reset at BEGIN): the bound group, whether the
  // transaction ever touched another group, and whether it ever wrote.
  std::optional<uint64_t> bound_group_;
  bool crossed_groups_ = false;
  bool wrote_ = false;
  bool loose_cross_group_reads_ = false;
};

} // namespace raft
