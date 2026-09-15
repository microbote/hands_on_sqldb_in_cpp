#pragma once

#include <vector>

#include "raft/log_store.h"

namespace raft {

// In-memory LogStore for deterministic P0 tests. It is not a production
// implementation.
class MemoryLogStore final : public LogStore {
public:
  std::expected<void, Error> append(const LogEntry &entry) override {
    if (entry.index == kInvalidIndex || entry.term == 0) {
      return std::unexpected(
          Error{ErrorCode::InvalidArgument, "invalid log entry"});
    }
    if (entry.index != last_index_ + 1) {
      return std::unexpected(Error{
          ErrorCode::InvalidArgument,
          "append must extend the log by exactly one entry"});
    }
    entries_.push_back(entry);
    last_index_ = entry.index;
    return {};
  }

  std::expected<LogEntry, Error> at(uint64_t index) const override {
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
      // The snapshot boundary: the entry's term is known, its data is not
      // stored (it lives inside the state machine snapshot).
      return LogEntry{index, snapshot_meta_.last_included_term, {}};
    }
    return entries_[static_cast<size_t>(index - 1 -
                                        snapshot_meta_.last_included_index)];
  }

  std::expected<void, Error> truncate_suffix(uint64_t from) override {
    if (from == kInvalidIndex || from <= snapshot_meta_.last_included_index ||
        from > last_index_ + 1) {
      return std::unexpected(
          Error{ErrorCode::InvalidArgument, "truncate suffix out of range"});
    }
    const size_t position =
        static_cast<size_t>(from - 1 - snapshot_meta_.last_included_index);
    entries_.erase(entries_.begin() + static_cast<ptrdiff_t>(position),
                   entries_.end());
    last_index_ = from - 1;
    return {};
  }

  std::expected<void, Error>
  save_hard_state(const HardState &hard_state) override {
    hard_state_ = hard_state;
    ++hard_state_writes_;
    return {};
  }

  std::expected<HardState, Error> load_hard_state() const override {
    return hard_state_;
  }

  std::expected<uint64_t, Error> last_index() const override {
    return last_index_;
  }

  std::expected<SnapshotMetadata, Error> snapshot_metadata() const override {
    return snapshot_meta_;
  }

  std::expected<void, Error>
  install_snapshot(const SnapshotMetadata &meta) override {
    if (meta.last_included_index == kInvalidIndex ||
        meta.last_included_term == 0) {
      return std::unexpected(Error{
          ErrorCode::InvalidArgument, "invalid snapshot metadata"});
    }
    // Entries at or below the new snapshot boundary are superseded by the
    // snapshot; drop them from the in-memory log.
    while (!entries_.empty() &&
           entries_.front().index <= meta.last_included_index) {
      entries_.erase(entries_.begin());
    }
    snapshot_meta_ = meta;
    if (last_index_ < meta.last_included_index) {
      last_index_ = meta.last_included_index;
    }
    return {};
  }

  size_t hard_state_writes() const { return hard_state_writes_; }
  size_t size() const { return entries_.size(); }

private:
  std::vector<LogEntry> entries_;
  uint64_t last_index_ = kInvalidIndex;
  SnapshotMetadata snapshot_meta_{};
  HardState hard_state_{};
  size_t hard_state_writes_ = 0;
};

} // namespace raft
