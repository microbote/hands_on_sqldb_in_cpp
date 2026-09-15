#include "raft/raft_kv_store.h"

#include <atomic>
#include <chrono>
#include <iterator>
#include <random>
#include <set>
#include <stdexcept>
#include <utility>

#include "raft/proposal_payload.h"
#include "storage/kv_engine/merging_iterator.h"
#include "storage/kv_engine/tx_buffer.h"

namespace raft {
namespace {

kv::Status error_to_status(const Error &error) {
  switch (error.code) {
  case ErrorCode::NotLeader:
    return kv::Status::NotLeader;
  case ErrorCode::Busy:
    // The raft service queue is full or the call came from the wrong thread:
    // retryable, exactly like the per-group write slot being taken.
    return kv::Status::Busy;
  case ErrorCode::InvalidArgument:
    return kv::Status::InvalidArgument;
  case ErrorCode::Timeout:
    return kv::Status::Timeout;
  case ErrorCode::IOError:
    return kv::Status::IOError;
  case ErrorCode::InternalError:
    break;
  }
  return kv::Status::InternalError;
}

// Each connection needs a client id that is unique across process restarts.
//
// The request-result store is durable and the state machine skips a proposal
// whose (client_id, request_id) was already applied. A plain per-process
// counter starting at 1 would repeat after a restart, and a brand-new write
// could then be mistaken for a replay of an old one and silently skipped.
// Random per-process salt + counter makes that collision negligible while
// keeping ids cheap to produce.
uint64_t allocate_client_id() {
  static std::atomic<uint64_t> counter{0};
  static const uint64_t process_salt = [] {
    std::random_device device;
    return (static_cast<uint64_t>(device()) << 32) ^
           static_cast<uint64_t>(device());
  }();
  return process_salt + counter.fetch_add(1);
}

} // namespace

RaftKVStore::RaftKVStore(std::shared_ptr<kv::KVStore> local,
                         RaftExecutor &executor,
                         std::map<uint64_t, std::string> client_endpoints)
    : RaftKVStore(std::move(local),
                  std::map<uint64_t, RaftExecutor *>{{0, &executor}},
                  GroupRouter{}, std::move(client_endpoints)) {}

RaftKVStore::RaftKVStore(std::shared_ptr<kv::KVStore> local,
                         std::map<uint64_t, RaftExecutor *> executors,
                         GroupRouter router,
                         std::map<uint64_t, std::string> client_endpoints)
    : local_(std::move(local)), executors_(std::move(executors)),
      router_(std::move(router)),
      client_endpoints_(std::move(client_endpoints)) {}

RaftKVStore::~RaftKVStore() = default;

kv::Status RaftKVStore::open(const kv::DatabaseOptions &options) {
  // Startup normally opens the local store first (it is the replicated state
  // machine's storage), so tolerate an already-open store.
  if (local_->is_open()) {
    return kv::Status::OK;
  }
  return local_->open(options);
}

kv::Status RaftKVStore::close() { return local_->close(); }

bool RaftKVStore::is_open() const { return local_->is_open(); }

std::shared_ptr<kv::KVEngine> RaftKVStore::connect() {
  if (!local_->is_open()) {
    return nullptr;
  }
  auto local_engine = local_->connect();
  if (local_engine == nullptr) {
    return nullptr;
  }
  return std::make_shared<RaftKVEngine>(shared_from_this(),
                                        std::move(local_engine),
                                        allocate_client_id());
}

void RaftKVStore::flush() { local_->flush(); }

std::string RaftKVStore::stats() const { return local_->stats(); }

bool RaftKVStore::write_slot_held() const {
  for (const auto &[group, slot] : write_slots_) {
    if (slot.held) {
      return true;
    }
  }
  return false;
}

std::optional<kv::LeaderHint> RaftKVStore::leader_hint() {
  return leader_hint_for_group(0);
}

std::optional<kv::LeaderHint>
RaftKVStore::leader_hint_for_group(uint64_t group) const {
  RaftExecutor &executor = executor_for(group);
  const std::optional<NodeId> leader = executor.leader_hint();
  if (!leader.has_value() || *leader == executor.node_id()) {
    // No hint, or we *are* the leader: nothing to redirect.
    return std::nullopt;
  }
  kv::LeaderHint hint;
  hint.node_id = leader->value;
  const auto endpoint = client_endpoints_.find(leader->value);
  if (endpoint != client_endpoints_.end()) {
    hint.endpoint = endpoint->second;
  }
  return hint;
}

kv::Status RaftKVStore::write_batch(const kv::WriteBatch &) {
  // The raw store-level write path exists for the replicated state machine,
  // which owns the local store directly. SQL writes must go through
  // connect() so they are proposed, committed and applied to every replica.
  return kv::Status::NotSupported;
}

std::unique_ptr<kv::Iterator>
RaftKVStore::new_iterator(const kv::KeyRange &range) {
  // Reads of the applied state are safe to serve directly; they are not a
  // write path. Session reads go through RaftKVEngine, which adds the read
  // barrier and the connection's transaction overlay.
  return local_->new_iterator(range);
}

RaftExecutor &RaftKVStore::executor_for(uint64_t group) const {
  const auto it = executors_.find(group);
  if (it == executors_.end()) {
    // Configuration bug: surface it loudly instead of silently misrouting.
    throw std::out_of_range("no raft executor for group " +
                            std::to_string(group));
  }
  return *it->second;
}

bool RaftKVStore::acquire_write_slot(uint64_t group, const void *owner) {
  WriteSlot &slot = write_slots_[group];
  if (slot.held) {
    return false;
  }
  slot.held = true;
  slot.owner = owner;
  return true;
}

void RaftKVStore::release_write_slot(uint64_t group, const void *owner) {
  const auto it = write_slots_.find(group);
  if (it == write_slots_.end() || it->second.owner != owner) {
    return;
  }
  it->second.held = false;
  it->second.owner = nullptr;
}

RaftKVEngine::RaftKVEngine(std::shared_ptr<RaftKVStore> store,
                           std::shared_ptr<kv::KVEngine> local_engine,
                           uint64_t client_id)
    : store_(std::move(store)), local_engine_(std::move(local_engine)),
      client_id_(client_id) {}

RaftKVEngine::~RaftKVEngine() {
  tx_.reset();
  if (write_slot_) {
    store_->release_write_slot(*write_group_, this);
  }
}

std::shared_ptr<kv::KVStore> RaftKVEngine::store() const { return store_; }

bool RaftKVEngine::is_open() const { return store_->is_open(); }

kv::Status RaftKVEngine::ensure_readable(uint64_t group) {
  // A transaction that still holds its fixed snapshot was proven readable at
  // begin_transaction() (one barrier per group, taken before the snapshot);
  // every read inside it reads the same snapshot, so they all reuse those
  // barriers instead of paying one heartbeat round each. Once the first write
  // releases the snapshot, reads go back to fresh barriers.
  if (tx_ != nullptr && local_engine_->has_snapshot()) {
    return last_error_ = kv::Status::OK;
  }
  RaftExecutor &executor = store_->executor_for(group);
  auto read_index = executor.read_barrier();
  if (!read_index.has_value()) {
    return last_error_ = error_to_status(read_index.error());
  }
  // A leader that cannot reach a quorum within its read budget has lost its
  // proof of leadership, so the read must fail instead of waiting forever.
  auto ready = read_index->wait_for(
      std::chrono::milliseconds{executor.read_timeout_ms()});
  if (!ready.has_value()) {
    return last_error_ = error_to_status(ready.error());
  }
  return last_error_ = kv::Status::OK;
}

kv::Status RaftKVEngine::check_group(uint64_t group, bool is_write) {
  if (!bound_group_.has_value()) {
    bound_group_ = group;
    return kv::Status::OK;
  }
  if (*bound_group_ != group) {
    cross_group_from_ = *bound_group_;
    cross_group_to_ = group;
    if (is_write) {
      return last_error_ = kv::Status::CrossGroupTransaction;
    }
    if (wrote_) {
      // "指向别的 group，且本事务已经写过": a write transaction never crosses.
      return last_error_ = kv::Status::CrossGroupTransaction;
    }
    crossed_groups_ = true;
    if (!loose_cross_group_reads_) {
      return last_error_ = kv::Status::CrossGroupTransaction;
    }
    return kv::Status::OK;
  }
  if (crossed_groups_ && is_write) {
    // "已经跨过组，再想写": a crossed transaction is frozen read-only.
    // The offending group is the bound group itself; keep the generic message
    // (no from/to pair) since this is not a two-group write conflict.
    return last_error_ = kv::Status::CrossGroupTransaction;
  }
  return kv::Status::OK;
}

kv::Status RaftKVEngine::get(const kv::Key &key, kv::ByteValue *value) {
  const uint64_t group = store_->router().group_for(key);
  // @system/* 是元数据：读它不算"碰数据"，不绑定事务组，也不触发跨组规则
  // （DML 执行期会读 schema，不能因此把自动提交事务绑到组 0）。
  if (tx_ != nullptr && !store_->router().is_system_key(key)) {
    if (const kv::Status status = check_group(group, /*is_write=*/false);
        status != kv::Status::OK) {
      return status;
    }
  }
  if (const kv::Status status = ensure_readable(group);
      status != kv::Status::OK) {
    return status;
  }
  return read_key(key, value);
}

kv::Status RaftKVEngine::read_key(const kv::Key &key, kv::ByteValue *value) {
  // Read through this connection's transaction first: a transaction must see
  // its own buffered writes and deletes, exactly like the local engine does.
  if (tx_ != nullptr) {
    const kv::OverlayOp op = tx_->lookup(key);
    if (op.is_tombstone()) {
      return kv::Status::NotFound;
    }
    if (op.has_value()) {
      if (value != nullptr) {
        *value = op.value;
      }
      return kv::Status::OK;
    }
  }

  auto iterator = make_iterator(kv::KeyRange::from(key));
  if (iterator == nullptr) {
    return last_error_ = kv::Status::InternalError;
  }
  if (!iterator->valid() || iterator->key() != key) {
    return kv::Status::NotFound;
  }
  if (value != nullptr) {
    *value = iterator->value();
  }
  return kv::Status::OK;
}

kv::Status RaftKVEngine::put(const kv::Key &key,
                             const kv::ByteValue &value) {
  kv::WriteBatch batch;
  batch.put(key, value);
  return write_batch(batch);
}

kv::Status RaftKVEngine::remove(const kv::Key &key) {
  kv::WriteBatch batch;
  batch.remove(key);
  return write_batch(batch);
}

bool RaftKVEngine::exists(const kv::Key &key) {
  const uint64_t group = store_->router().group_for(key);
  if (tx_ != nullptr && !store_->router().is_system_key(key)) {
    if (check_group(group, /*is_write=*/false) != kv::Status::OK) {
      return false;
    }
  }
  if (ensure_readable(group) != kv::Status::OK) {
    return false;
  }
  kv::ByteValue ignored;
  return read_key(key, &ignored) == kv::Status::OK;
}

kv::Status RaftKVEngine::get_batch(
    const std::vector<kv::Key> &keys, kv::MissingKeyPolicy policy,
    std::vector<std::optional<kv::ByteValue>> *values) {
  if (values == nullptr) {
    return kv::Status::InvalidArgument;
  }

  // Route: a batch spanning groups is a cross-group read. In a transaction it
  // goes through the strict/loose rules; outside one, a single statement must
  // stay within one group (one table), so reject it.
  // @system/* 键不参与事务绑定（元数据读不算碰数据）。
  std::set<uint64_t> groups;
  for (const kv::Key &key : keys) {
    if (!store_->router().is_system_key(key)) {
      groups.insert(store_->router().group_for(key));
    }
  }
  if (groups.size() > 1 && tx_ == nullptr) {
    const auto first = groups.begin();
    cross_group_from_ = *first;
    cross_group_to_ = *std::next(first);
    return last_error_ = kv::Status::CrossGroupTransaction;
  }
  for (const uint64_t group : groups) {
    if (tx_ != nullptr) {
      if (const kv::Status status = check_group(group, /*is_write=*/false);
          status != kv::Status::OK) {
        return status;
      }
    }
    if (const kv::Status status = ensure_readable(group);
        status != kv::Status::OK) {
      return status;
    }
  }

  values->clear();
  values->reserve(keys.size());

  // Phase 1: keys served from the transaction overlay are resolved directly;
  // the rest need the store.
  std::vector<size_t> misses;
  for (size_t i = 0; i < keys.size(); ++i) {
    bool handled = false;
    if (tx_ != nullptr) {
      const kv::OverlayOp op = tx_->lookup(keys[i]);
      if (op.is_tombstone()) {
        if (policy == kv::MissingKeyPolicy::kReturnError) {
          return last_error_ = kv::Status::NotFound;
        }
        values->push_back(std::nullopt);
        handled = true;
      } else if (op.has_value()) {
        values->push_back(op.value);
        handled = true;
      }
    }
    if (!handled) {
      misses.push_back(i);
      values->push_back(std::nullopt); // placeholder, filled below
    }
  }
  if (misses.empty()) {
    return last_error_ = kv::Status::OK;
  }

  // Phase 2: one iterator, seek per key. Avoids allocating and registering a
  // fresh iterator (plus its MergingIterator) for every key.
  auto iterator = make_iterator(kv::KeyRange::all());
  if (iterator == nullptr) {
    return last_error_ = kv::Status::InternalError;
  }
  for (const size_t i : misses) {
    iterator->seek(keys[i]);
    if (iterator->valid() && iterator->key() == keys[i]) {
      (*values)[i] = iterator->value();
    } else if (policy == kv::MissingKeyPolicy::kReturnError) {
      return last_error_ = kv::Status::NotFound;
    }
  }
  return last_error_ = kv::Status::OK;
}

kv::Status RaftKVEngine::write_batch(const kv::WriteBatch &batch) {
  if (!store_->is_open()) {
    return kv::Status::InternalError;
  }
  if (batch.empty()) {
    return kv::Status::OK;
  }

  // Route: every op must land in one group. put/remove mismatches record the
  // two groups; a remove_range spanning groups keeps the generic message (its
  // boundary is not a single "second group").
  std::optional<uint64_t> group;
  for (const auto &op : batch.ops()) {
    std::optional<uint64_t> op_group;
    if (op.type == kv::WriteBatch::OpType::kRemoveRange) {
      op_group = store_->router().range_group(op.data.key, op.range_end);
      if (!op_group.has_value()) {
        return last_error_ = kv::Status::CrossGroupTransaction;
      }
    } else {
      op_group = store_->router().group_for(op.data.key);
    }
    if (!group.has_value()) {
      group = op_group;
    } else if (*group != *op_group) {
      cross_group_from_ = *group;
      cross_group_to_ = *op_group;
      return last_error_ = kv::Status::CrossGroupTransaction;
    }
  }

  if (tx_ == nullptr) {
    return propose_batch(*group, batch);
  }

  if (const kv::Status status = check_group(*group, /*is_write=*/true);
      status != kv::Status::OK) {
    return status;
  }
  const kv::Status slot = acquire_write_slot_for(*group);
  if (slot != kv::Status::OK) {
    return slot;
  }
  for (const auto &op : batch.ops()) {
    switch (op.type) {
    case kv::WriteBatch::OpType::kPut:
      if (op.data.value.has_value()) {
        tx_->put(op.data.key, *op.data.value);
      }
      break;
    case kv::WriteBatch::OpType::kRemove:
      tx_->remove(op.data.key);
      break;
    case kv::WriteBatch::OpType::kRemoveRange:
      tx_->remove_range(op.data.key, op.range_end);
      break;
    }
  }
  wrote_ = true;
  return kv::Status::OK;
}

std::unique_ptr<kv::Iterator>
RaftKVEngine::new_iterator(const kv::KeyRange &range) {
  if (!store_->is_open()) {
    last_error_ = kv::Status::InternalError;
    return nullptr;
  }
  const kv::Key start = range.start.value_or(kv::Key{});
  const auto group = store_->router().range_group(start, range.end);
  if (!group.has_value()) {
    // The scan would cross a group boundary. One table = one group, so a
    // single statement should never need this; full cross-group scan routing
    // is future work.
    last_error_ = kv::Status::CrossGroupTransaction;
    return nullptr;
  }
  if (tx_ != nullptr && !store_->router().is_system_key(start)) {
    if (const kv::Status status = check_group(*group, /*is_write=*/false);
        status != kv::Status::OK) {
      last_error_ = status;
      return nullptr;
    }
  }
  if (const kv::Status status = ensure_readable(*group);
      status != kv::Status::OK) {
    last_error_ = status;
    return nullptr;
  }
  auto iterator = make_iterator(range);
  if (iterator == nullptr) {
    last_error_ = kv::Status::InternalError;
  }
  return iterator;
}

kv::Status RaftKVEngine::begin_transaction() {
  if (!store_->is_open()) {
    return kv::Status::InternalError;
  }
  if (tx_ != nullptr) {
    return kv::Status::Busy;
  }
  // Prove leadership and applied-index for every group first: the snapshot
  // taken afterwards must include everything committed before the transaction
  // began. This is also the set of barriers the whole (read-only) transaction
  // reuses.
  for (const auto &[group, executor] : store_->executors()) {
    (void)executor;
    if (const kv::Status status = ensure_readable(group);
        status != kv::Status::OK) {
      return status;
    }
  }
  const kv::Status status = local_engine_->begin_transaction();
  if (status != kv::Status::OK) {
    return status;
  }
  tx_ = std::make_unique<kv::TxBuffer>();
  bound_group_.reset();
  crossed_groups_ = false;
  wrote_ = false;
  return kv::Status::OK;
}

kv::Status RaftKVEngine::commit_transaction() {
  if (!store_->is_open()) {
    return kv::Status::InternalError;
  }
  if (tx_ == nullptr) {
    return kv::Status::NotFound;
  }
  if (tx_->exceeds()) {
    return kv::Status::InvalidArgument;
  }

  if (tx_->empty()) {
    tx_.reset();
    release_local_snapshot();
    if (write_slot_) {
      release_write_slot();
    }
    return kv::Status::OK;
  }

  // Buffered writes never cross groups (check_group rejects them), so the
  // write slot's group is the proposal group.
  if (!write_group_.has_value()) {
    return last_error_ = kv::Status::InternalError;
  }
  kv::WriteBatch batch = tx_->to_batch();
  batch.set_sync(true);
  const kv::Status status = propose_batch(*write_group_, batch);
  if (status != kv::Status::OK) {
    return status;
  }

  tx_.reset();
  if (local_engine_->has_snapshot()) {
    release_local_snapshot();
  }
  if (write_slot_) {
    release_write_slot();
  }
  return kv::Status::OK;
}

kv::Status RaftKVEngine::rollback_transaction() {
  if (tx_ == nullptr) {
    return kv::Status::NotFound;
  }
  tx_.reset();
  release_local_snapshot();
  if (write_slot_) {
    release_write_slot();
  }
  return kv::Status::OK;
}

bool RaftKVEngine::in_transaction() const { return tx_ != nullptr; }

kv::Status RaftKVEngine::acquire_write_slot() {
  if (write_slot_) {
    return kv::Status::OK;
  }
  // 无参版本兼容旧调用：事务已绑组则抢该组的槽，否则默认 0 号组。多组
  // server 路径用带组版本（session 先按语句目标算组）。
  const uint64_t group = bound_group_.value_or(0);
  return acquire_write_slot_for(group);
}

kv::Status RaftKVEngine::acquire_write_slot(uint64_t group) {
  return acquire_write_slot_for(group);
}

void RaftKVEngine::release_write_slot() {
  if (!write_slot_) {
    return;
  }
  store_->release_write_slot(*write_group_, this);
  write_slot_ = false;
  write_group_.reset();
}

bool RaftKVEngine::has_write_slot() const { return write_slot_; }

bool RaftKVEngine::has_snapshot() const {
  return tx_ != nullptr && local_engine_->has_snapshot();
}

void RaftKVEngine::flush() { store_->flush(); }

std::string RaftKVEngine::stats() const { return store_->stats(); }

std::string RaftKVEngine::name() const { return "RaftKVEngine"; }

kv::Status RaftKVEngine::acquire_write_slot_for(uint64_t group) {
  if (write_slot_) {
    return kv::Status::OK;
  }
  if (!store_->acquire_write_slot(group, this)) {
    return last_error_ = kv::Status::Busy;
  }
  write_slot_ = true;
  write_group_ = group;
  if (local_engine_->has_snapshot()) {
    release_local_snapshot();
  }
  return kv::Status::OK;
}

kv::Status RaftKVEngine::propose_batch(uint64_t group,
                                       const kv::WriteBatch &batch) {
  if (write_slot_ && write_group_.has_value() && *write_group_ != group) {
    // A slot for another group is held (e.g. the session pre-acquired group
    // 0's slot and the statement routed elsewhere): refuse instead of writing
    // under the wrong group's slot.
    return last_error_ = kv::Status::CrossGroupTransaction;
  }
  if (!write_slot_) {
    if (!store_->acquire_write_slot(group, this)) {
      return last_error_ = kv::Status::Busy;
    }
    write_slot_ = true;
    write_group_ = group;
  }
  if (tx_ != nullptr && local_engine_->has_snapshot()) {
    release_local_snapshot();
  }

  ProposalPayload payload;
  payload.client_id = client_id_;
  payload.request_id = next_request_id_++;
  payload.batch = batch;

  RaftExecutor &executor = store_->executor_for(group);
  auto proposal = executor.propose(encode_proposal_payload(payload));
  if (!proposal.has_value()) {
    if (tx_ == nullptr) {
      release_write_slot();
    }
    return last_error_ = error_to_status(proposal.error());
  }
  // Timeout here means "unknown outcome", not "not applied": the entry may
  // still commit later. Callers retry with the same client/request id, which
  // the state machine deduplicates.
  auto committed = proposal->wait_for(std::chrono::milliseconds{
      executor.proposal_timeout_ms()});
  if (!committed.has_value()) {
    if (tx_ == nullptr) {
      release_write_slot();
    }
    return last_error_ = error_to_status(committed.error());
  }
  if (tx_ == nullptr) {
    release_write_slot();
  }
  return last_error_ = kv::Status::OK;
}

void RaftKVEngine::release_local_snapshot() {
  if (local_engine_->in_transaction()) {
    (void)local_engine_->rollback_transaction();
  }
}

std::unique_ptr<kv::Iterator>
RaftKVEngine::make_iterator(const kv::KeyRange &range) {
  auto inner = local_engine_->new_iterator(range);
  if (inner == nullptr) {
    return nullptr;
  }
  if (tx_ == nullptr) {
    return inner;
  }
  return std::make_unique<kv::MergingIterator>(
      std::move(inner), tx_.get(), range);
}

} // namespace raft
