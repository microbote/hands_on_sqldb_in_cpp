#pragma once

#if defined(SQLDB_HAVE_LEVELDB)

#include <expected>
#include <memory>
#include <mutex>
#include <optional>
#include <string>

#include <leveldb/db.h>

#include "raft/request_result_store.h"

namespace raft {

class LevelDBRequestResultStore final : public RequestResultStore {
public:
  LevelDBRequestResultStore() = default;
  ~LevelDBRequestResultStore() override;

  LevelDBRequestResultStore(const LevelDBRequestResultStore &) = delete;
  LevelDBRequestResultStore &
  operator=(const LevelDBRequestResultStore &) = delete;

  std::expected<void, Error> open(const std::string &path);
  std::expected<void, Error> close();

  std::expected<std::optional<std::string>, Error>
  find(uint64_t client_id, uint64_t request_id) const override;

  std::expected<void, Error>
  save(uint64_t client_id, uint64_t request_id,
       std::string result) override;

private:
  mutable std::mutex mutex_;
  std::unique_ptr<leveldb::DB> db_;
};

} // namespace raft

#endif
