#include "raft/leveldb_log_store.h"

#if defined(SQLDB_HAVE_LEVELDB)

#include <leveldb/write_batch.h>

#include <array>
#include <cstddef>
#include <string_view>
#include <utility>

namespace raft {
namespace {

constexpr std::string_view kHardStateKey = "hard_state";
constexpr std::string_view kLogPrefix = "log/";
constexpr std::string_view kSnapshotMetaKey = "snapshot_meta";
constexpr size_t kEncodedIndexSize = sizeof(uint64_t);
constexpr size_t kEncodedHardStateSize = kEncodedIndexSize + 1 +
                                         kEncodedIndexSize;
constexpr size_t kEncodedSnapshotMetaSize = 2 * kEncodedIndexSize;

void append_u64(std::string &out, uint64_t value) {
  for (int shift = 56; shift >= 0; shift -= 8) {
    const auto byte = static_cast<unsigned char>((value >> shift) & 0xff);
    out.push_back(static_cast<char>(byte));
  }
}

std::expected<uint64_t, Error> decode_u64(std::string_view value,
                                           size_t &offset) {
  if (value.size() < offset + kEncodedIndexSize) {
    return std::unexpected(
        Error{ErrorCode::InternalError, "truncated uint64 value"});
  }
  uint64_t result = 0;
  for (size_t i = 0; i < kEncodedIndexSize; ++i) {
    result = (result << 8) |
             static_cast<uint64_t>(
                 static_cast<unsigned char>(value[offset + i]));
  }
  offset += kEncodedIndexSize;
  return result;
}

std::string log_key(uint64_t index) {
  std::string key{kLogPrefix};
  append_u64(key, index);
  return key;
}

bool is_log_key(std::string_view key) {
  return key.size() == kLogPrefix.size() + kEncodedIndexSize &&
         key.substr(0, kLogPrefix.size()) == kLogPrefix;
}

std::expected<uint64_t, Error> decode_log_index(std::string_view key) {
  if (!is_log_key(key)) {
    return std::unexpected(
        Error{ErrorCode::InternalError, "unexpected raft log key"});
  }
  size_t offset = kLogPrefix.size();
  return decode_u64(key, offset);
}

std::expected<void, Error> check_db(const leveldb::DB *db) {
  if (db == nullptr) {
    return std::unexpected(
        Error{ErrorCode::InternalError, "raft log store is not open"});
  }
  return {};
}

Error leveldb_error(const leveldb::Status &status) {
  return Error{ErrorCode::IOError, status.ToString()};
}

} // namespace

LevelDBLogStore::~LevelDBLogStore() {
  (void)close();
}

std::expected<void, Error> LevelDBLogStore::open(const std::string &path) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (db_ != nullptr) {
    return std::unexpected(
        Error{ErrorCode::InvalidArgument, "raft log store is already open"});
  }

  leveldb::Options options;
  options.create_if_missing = true;
  options.paranoid_checks = true;

  leveldb::DB *db = nullptr;
  const leveldb::Status status = leveldb::DB::Open(options, path, &db);
  if (!status.ok()) {
    delete db;
    return std::unexpected(leveldb_error(status));
  }
  db_.reset(db);

  // Load snapshot metadata first: with compaction the log may start at
  // last_included_index + 1 instead of 1.
  std::string meta_value;
  const leveldb::Status meta_status = db_->Get(
      leveldb::ReadOptions(), std::string{kSnapshotMetaKey}, &meta_value);
  if (meta_status.IsNotFound()) {
    // No snapshot yet.
  } else if (!meta_status.ok()) {
    return std::unexpected(leveldb_error(meta_status));
  } else {
    if (meta_value.size() != kEncodedSnapshotMetaSize) {
      return std::unexpected(Error{
          ErrorCode::InternalError, "invalid raft snapshot metadata size"});
    }
    size_t meta_offset = 0;
    auto included = decode_u64(meta_value, meta_offset);
    auto included_term = decode_u64(meta_value, meta_offset);
    if (!included.has_value() || !included_term.has_value()) {
      return std::unexpected(Error{
          ErrorCode::InternalError, "invalid raft snapshot metadata"});
    }
    if (*included == kInvalidIndex || *included_term == 0) {
      return std::unexpected(Error{
          ErrorCode::InternalError, "invalid raft snapshot metadata values"});
    }
    snapshot_meta_ = SnapshotMetadata{*included, *included_term};
  }

  // Log entries must be contiguous from last_included_index + 1. A key at or
  // below the snapshot boundary is a stale prefix left by a crash between
  // "state machine restored" and "log compacted"; drop it as recovery.
  const uint64_t expected_start =
      snapshot_meta_.has_snapshot() ? snapshot_meta_.last_included_index + 1
                                    : 1;
  uint64_t expected = expected_start;
  uint64_t last_found = kInvalidIndex;
  leveldb::WriteBatch recovery_batch;
  bool has_recovery = false;
  std::unique_ptr<leveldb::Iterator> it(
      db_->NewIterator(leveldb::ReadOptions()));
  for (it->Seek(log_key(1)); it->Valid() && is_log_key(it->key().ToString());
       it->Next()) {
    const auto index = decode_log_index(it->key().ToString());
    if (!index.has_value()) {
      return std::unexpected(index.error());
    }
    if (*index < expected_start) {
      recovery_batch.Delete(it->key());
      has_recovery = true;
      continue;
    }
    if (*index != expected) {
      return std::unexpected(Error{
          ErrorCode::InternalError,
          "raft log store has a gap or a non-contiguous prefix"});
    }
    if (it->value().size() < kEncodedIndexSize) {
      return std::unexpected(
          Error{ErrorCode::InternalError, "truncated raft log entry"});
    }
    last_found = *index;
    ++expected;
  }
  if (!it->status().ok()) {
    return std::unexpected(leveldb_error(it->status()));
  }

  if (has_recovery) {
    leveldb::WriteOptions options;
    options.sync = true;
    const leveldb::Status status = db_->Write(options, &recovery_batch);
    if (!status.ok()) {
      return std::unexpected(leveldb_error(status));
    }
  }

  last_index_ = std::max(last_found, snapshot_meta_.last_included_index);
  return {};
}

std::expected<void, Error> LevelDBLogStore::close() {
  std::lock_guard<std::mutex> lock(mutex_);
  if (db_ == nullptr) {
    return {};
  }
  db_.reset();
  last_index_ = kInvalidIndex;
  return {};
}

std::expected<void, Error> LevelDBLogStore::append(const LogEntry &entry) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (auto result = check_db(db_.get()); !result.has_value()) {
    return std::unexpected(result.error());
  }
  if (entry.index == kInvalidIndex || entry.term == 0) {
    return std::unexpected(
        Error{ErrorCode::InvalidArgument, "invalid log entry"});
  }
  if (entry.index != last_index_ + 1) {
    return std::unexpected(Error{
        ErrorCode::InvalidArgument,
        "append must extend the log by exactly one entry"});
  }

  std::string value;
  append_u64(value, entry.term);
  value.append(entry.data);

  leveldb::WriteOptions options;
  options.sync = true;
  const leveldb::Status status =
      db_->Put(options, log_key(entry.index), value);
  if (!status.ok()) {
    return std::unexpected(leveldb_error(status));
  }

  last_index_ = entry.index;
  return {};
}

std::expected<LogEntry, Error>
LevelDBLogStore::at(uint64_t index) const {
  std::lock_guard<std::mutex> lock(mutex_);
  if (auto result = check_db(db_.get()); !result.has_value()) {
    return std::unexpected(result.error());
  }
  if (index == kInvalidIndex || index > last_index_) {
    return std::unexpected(
        Error{ErrorCode::InvalidArgument, "log index out of range"});
  }
  if (index < snapshot_meta_.last_included_index) {
    return std::unexpected(Error{
        ErrorCode::InvalidArgument,
        "log index is inside the compacted snapshot prefix"});
  }
  if (index == snapshot_meta_.last_included_index) {
    // The snapshot boundary: the entry's term is known, its data lives inside
    // the state machine snapshot and is not stored in the log.
    return LogEntry{index, snapshot_meta_.last_included_term, {}};
  }

  std::string value;
  const leveldb::Status status =
      db_->Get(leveldb::ReadOptions(), log_key(index), &value);
  if (status.IsNotFound()) {
    return std::unexpected(
        Error{ErrorCode::InternalError, "cached raft log index is missing"});
  }
  if (!status.ok()) {
    return std::unexpected(leveldb_error(status));
  }
  if (value.size() < kEncodedIndexSize) {
    return std::unexpected(
        Error{ErrorCode::InternalError, "truncated raft log entry"});
  }

  size_t offset = 0;
  auto term = decode_u64(value, offset);
  if (!term.has_value()) {
    return std::unexpected(term.error());
  }
  if (term == 0) {
    return std::unexpected(
        Error{ErrorCode::InternalError, "raft log entry has zero term"});
  }

  return LogEntry{index, *term, value.substr(offset)};
}

std::expected<void, Error>
LevelDBLogStore::truncate_suffix(uint64_t from) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (auto result = check_db(db_.get()); !result.has_value()) {
    return std::unexpected(result.error());
  }
  if (from == kInvalidIndex || from <= snapshot_meta_.last_included_index ||
      from > last_index_ + 1) {
    return std::unexpected(
        Error{ErrorCode::InvalidArgument, "truncate suffix out of range"});
  }

  leveldb::WriteBatch batch;
  std::unique_ptr<leveldb::Iterator> it(
      db_->NewIterator(leveldb::ReadOptions()));
  for (it->Seek(log_key(from));
       it->Valid() && is_log_key(it->key().ToString());
       it->Next()) {
    batch.Delete(it->key());
  }
  if (!it->status().ok()) {
    return std::unexpected(leveldb_error(it->status()));
  }

  leveldb::WriteOptions options;
  options.sync = true;
  const leveldb::Status status = db_->Write(options, &batch);
  if (!status.ok()) {
    return std::unexpected(leveldb_error(status));
  }

  last_index_ = from - 1;
  return {};
}

std::expected<void, Error>
LevelDBLogStore::save_hard_state(const HardState &hard_state) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (auto result = check_db(db_.get()); !result.has_value()) {
    return std::unexpected(result.error());
  }

  std::string value;
  append_u64(value, hard_state.term);
  value.push_back(hard_state.voted_for.has_value() ? '\x01' : '\x00');
  if (hard_state.voted_for.has_value()) {
    append_u64(value, hard_state.voted_for->value);
  }

  leveldb::WriteOptions options;
  options.sync = true;
  const leveldb::Status status =
      db_->Put(options, std::string{kHardStateKey}, value);
  if (!status.ok()) {
    return std::unexpected(leveldb_error(status));
  }
  return {};
}

std::expected<HardState, Error>
LevelDBLogStore::load_hard_state() const {
  std::lock_guard<std::mutex> lock(mutex_);
  if (auto result = check_db(db_.get()); !result.has_value()) {
    return std::unexpected(result.error());
  }

  std::string value;
  const leveldb::Status status = db_->Get(
      leveldb::ReadOptions(), std::string{kHardStateKey}, &value);
  if (status.IsNotFound()) {
    return HardState{};
  }
  if (!status.ok()) {
    return std::unexpected(leveldb_error(status));
  }
  if (value.size() != kEncodedHardStateSize) {
    return std::unexpected(
        Error{ErrorCode::InternalError, "invalid raft hard state size"});
  }

  size_t offset = 0;
  auto term = decode_u64(value, offset);
  if (!term.has_value()) {
    return std::unexpected(term.error());
  }
  const bool has_vote = value[offset++] != '\x00';
  HardState hard_state;
  hard_state.term = *term;
  if (has_vote) {
    auto voted_for = decode_u64(value, offset);
    if (!voted_for.has_value()) {
      return std::unexpected(voted_for.error());
    }
    hard_state.voted_for = NodeId{*voted_for};
  }
  return hard_state;
}

std::expected<uint64_t, Error> LevelDBLogStore::last_index() const {
  std::lock_guard<std::mutex> lock(mutex_);
  if (auto result = check_db(db_.get()); !result.has_value()) {
    return std::unexpected(result.error());
  }
  return last_index_;
}

std::expected<SnapshotMetadata, Error>
LevelDBLogStore::snapshot_metadata() const {
  std::lock_guard<std::mutex> lock(mutex_);
  if (auto result = check_db(db_.get()); !result.has_value()) {
    return std::unexpected(result.error());
  }
  return snapshot_meta_;
}

std::expected<void, Error>
LevelDBLogStore::install_snapshot(const SnapshotMetadata &meta) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (auto result = check_db(db_.get()); !result.has_value()) {
    return std::unexpected(result.error());
  }
  if (meta.last_included_index == kInvalidIndex ||
      meta.last_included_term == 0) {
    return std::unexpected(
        Error{ErrorCode::InvalidArgument, "invalid snapshot metadata"});
  }

  leveldb::WriteBatch batch;
  std::unique_ptr<leveldb::Iterator> it(
      db_->NewIterator(leveldb::ReadOptions()));
  for (it->Seek(log_key(1)); it->Valid() && is_log_key(it->key().ToString());
       it->Next()) {
    const auto index = decode_log_index(it->key().ToString());
    if (!index.has_value()) {
      return std::unexpected(index.error());
    }
    if (*index <= meta.last_included_index) {
      batch.Delete(it->key());
    } else {
      break; // keys are sorted, everything after this is above the boundary
    }
  }
  if (!it->status().ok()) {
    return std::unexpected(leveldb_error(it->status()));
  }

  std::string value;
  append_u64(value, meta.last_included_index);
  append_u64(value, meta.last_included_term);
  batch.Put(std::string{kSnapshotMetaKey}, value);

  leveldb::WriteOptions options;
  options.sync = true;
  const leveldb::Status status = db_->Write(options, &batch);
  if (!status.ok()) {
    return std::unexpected(leveldb_error(status));
  }

  snapshot_meta_ = meta;
  if (last_index_ < meta.last_included_index) {
    last_index_ = meta.last_included_index;
  }
  return {};
}

} // namespace raft

#endif
