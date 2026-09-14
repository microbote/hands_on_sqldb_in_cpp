#pragma once

#if defined(SQLDB_HAVE_LEVELDB)

#include <expected>
#include <memory>
#include <mutex>
#include <string>

#include <leveldb/db.h>

#include "raft/log_store.h"

namespace leveldb {
class WriteBatch;
}

namespace raft {

// LevelDB-backed LogStore for one Raft group.
//
// Layout:
//   hard_state -> encoded(term, voted_for)
//   log/<index> -> encoded(term, data)
//
// The index is a fixed-width big-endian integer, so lexical order matches log
// order. This store is intentionally separate from the business KV directory.
class LevelDBLogStore final : public LogStore {
public:
  LevelDBLogStore() = default;
  ~LevelDBLogStore() override;

  LevelDBLogStore(const LevelDBLogStore &) = delete;
  LevelDBLogStore &operator=(const LevelDBLogStore &) = delete;

  std::expected<void, Error> open(const std::string &path);
  std::expected<void, Error> close();

  std::expected<void, Error> append(const LogEntry &entry) override;
  std::expected<LogEntry, Error> at(uint64_t index) const override;
  std::expected<void, Error> truncate_suffix(uint64_t from) override;
  std::expected<void, Error>
  save_hard_state(const HardState &hard_state) override;
  std::expected<HardState, Error> load_hard_state() const override;
  std::expected<uint64_t, Error> last_index() const override;

private:
  mutable std::mutex mutex_;
  std::unique_ptr<leveldb::DB> db_;
  uint64_t last_index_ = kInvalidIndex;
};

} // namespace raft

#endif
