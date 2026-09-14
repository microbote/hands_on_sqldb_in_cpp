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
    if (entry.index != entries_.size() + 1) {
      return std::unexpected(Error{
          ErrorCode::InvalidArgument,
          "append must extend the log by exactly one entry"});
    }
    entries_.push_back(entry);
    return {};
  }

  std::expected<LogEntry, Error> at(uint64_t index) const override {
    if (index == kInvalidIndex || index > entries_.size()) {
      return std::unexpected(
          Error{ErrorCode::InvalidArgument, "log index out of range"});
    }
    return entries_[static_cast<size_t>(index - 1)];
  }

  std::expected<void, Error> truncate_suffix(uint64_t from) override {
    if (from == kInvalidIndex || from > entries_.size() + 1) {
      return std::unexpected(
          Error{ErrorCode::InvalidArgument, "truncate suffix out of range"});
    }
    entries_.erase(entries_.begin() + static_cast<ptrdiff_t>(from - 1),
                   entries_.end());
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
    return entries_.size();
  }

  size_t hard_state_writes() const { return hard_state_writes_; }
  size_t size() const { return entries_.size(); }

private:
  std::vector<LogEntry> entries_;
  HardState hard_state_{};
  size_t hard_state_writes_ = 0;
};

} // namespace raft
