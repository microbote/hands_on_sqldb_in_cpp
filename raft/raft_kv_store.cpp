#include "raft/raft_kv_store.h"

#include <chrono>
#include <random>
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

RaftKVStore::RaftKVStore(std::shared_ptr<kv::KVStore> local, RaftNode &node)
    : local_(std::move(local)), node_(node) {}

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
  return std::make_shared<RaftKVEngine>(
      shared_from_this(), std::move(local_engine), allocate_client_id());
}

void RaftKVStore::flush() { local_->flush(); }

std::string RaftKVStore::stats() const { return local_->stats(); }

bool RaftKVStore::write_slot_held() const {
  return write_slot_held_.load();
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

bool RaftKVStore::acquire_write_slot(const void *owner) {
  bool expected = false;
  if (!write_slot_held_.compare_exchange_strong(expected, true)) {
    return false;
  }
  write_slot_owner_ = owner;
  return true;
}

void RaftKVStore::release_write_slot(const void *owner) {
  if (write_slot_owner_ != owner) {
    return;
  }
  write_slot_owner_ = nullptr;
  write_slot_held_.store(false);
}

RaftKVEngine::RaftKVEngine(std::shared_ptr<RaftKVStore> store,
                           std::shared_ptr<kv::KVEngine> local_engine,
                           uint64_t client_id)
    : store_(std::move(store)), local_engine_(std::move(local_engine)),
      node_(store_->node_), client_id_(client_id) {}

RaftKVEngine::~RaftKVEngine() {
  tx_.reset();
  if (write_slot_) {
    store_->release_write_slot(this);
  }
}

std::shared_ptr<kv::KVStore> RaftKVEngine::store() const { return store_; }

bool RaftKVEngine::is_open() const { return store_->is_open(); }

kv::Status RaftKVEngine::ensure_readable() {
  auto read_index = node_.read_barrier();
  if (!read_index.has_value()) {
    return error_to_status(read_index.error());
  }
  // A leader that cannot reach a quorum within an election timeout has lost
  // its proof of leadership, so the read must fail instead of waiting forever.
  auto ready = read_index->wait_for(std::chrono::milliseconds{
      node_.election_timeout_ms()});
  if (!ready.has_value()) {
    return error_to_status(ready.error());
  }
  return kv::Status::OK;
}

kv::Status RaftKVEngine::get(const kv::Key &key, kv::ByteValue *value) {
  if (const kv::Status status = ensure_readable(); status != kv::Status::OK) {
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
    return kv::Status::InternalError;
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
  if (ensure_readable() != kv::Status::OK) {
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
  if (const kv::Status status = ensure_readable(); status != kv::Status::OK) {
    return status;
  }
  values->clear();
  values->reserve(keys.size());
  for (const auto &key : keys) {
    kv::ByteValue value;
    const kv::Status status = read_key(key, &value);
    if (status == kv::Status::OK) {
      values->push_back(std::move(value));
    } else if (status == kv::Status::NotFound &&
               policy == kv::MissingKeyPolicy::kReturnEmpty) {
      values->push_back(std::nullopt);
    } else {
      return status;
    }
  }
  return kv::Status::OK;
}

kv::Status RaftKVEngine::write_batch(const kv::WriteBatch &batch) {
  if (!store_->is_open()) {
    return kv::Status::InternalError;
  }
  if (batch.empty()) {
    return kv::Status::OK;
  }

  if (tx_ == nullptr) {
    return propose_batch(batch);
  }

  const kv::Status slot = acquire_write_slot();
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
  return kv::Status::OK;
}

std::unique_ptr<kv::Iterator>
RaftKVEngine::new_iterator(const kv::KeyRange &range) {
  if (!store_->is_open()) {
    return nullptr;
  }
  if (const kv::Status status = ensure_readable(); status != kv::Status::OK) {
    return nullptr;
  }
  return make_iterator(range);
}

kv::Status RaftKVEngine::begin_transaction() {
  if (!store_->is_open()) {
    return kv::Status::InternalError;
  }
  if (tx_ != nullptr) {
    return kv::Status::Busy;
  }
  const kv::Status status = local_engine_->begin_transaction();
  if (status != kv::Status::OK) {
    return status;
  }
  tx_ = std::make_unique<kv::TxBuffer>();
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

  kv::WriteBatch batch = tx_->to_batch();
  batch.set_sync(true);
  const kv::Status status = propose_batch(batch);
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
  if (!store_->acquire_write_slot(this)) {
    return kv::Status::Busy;
  }
  write_slot_ = true;
  if (local_engine_->has_snapshot()) {
    release_local_snapshot();
  }
  return kv::Status::OK;
}

void RaftKVEngine::release_write_slot() {
  if (!write_slot_) {
    return;
  }
  store_->release_write_slot(this);
  write_slot_ = false;
}

bool RaftKVEngine::has_write_slot() const { return write_slot_; }

bool RaftKVEngine::has_snapshot() const {
  return tx_ != nullptr && local_engine_->has_snapshot();
}

void RaftKVEngine::flush() { store_->flush(); }

std::string RaftKVEngine::stats() const { return store_->stats(); }

std::string RaftKVEngine::name() const { return "RaftKVEngine"; }

kv::Status RaftKVEngine::propose_batch(const kv::WriteBatch &batch) {
  if (!write_slot_) {
    if (!store_->acquire_write_slot(this)) {
      return kv::Status::Busy;
    }
    write_slot_ = true;
  }
  if (tx_ != nullptr && local_engine_->has_snapshot()) {
    release_local_snapshot();
  }

  ProposalPayload payload;
  payload.client_id = client_id_;
  payload.request_id = next_request_id_++;
  payload.batch = batch;

  auto proposal = node_.propose(encode_proposal_payload(payload));
  if (!proposal.has_value()) {
    if (tx_ == nullptr) {
      release_write_slot();
    }
    return error_to_status(proposal.error());
  }
  // Timeout here means "unknown outcome", not "not applied": the entry may
  // still commit later. Callers retry with the same client/request id, which
  // the state machine deduplicates.
  auto committed = proposal->wait_for(std::chrono::milliseconds{
      node_.election_timeout_ms()});
  if (!committed.has_value()) {
    if (tx_ == nullptr) {
      release_write_slot();
    }
    return error_to_status(committed.error());
  }
  if (tx_ == nullptr) {
    release_write_slot();
  }
  return kv::Status::OK;
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
