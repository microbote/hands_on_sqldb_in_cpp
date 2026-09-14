#pragma once

#include <cstdint>
#include <expected>
#include <map>
#include <optional>
#include <string>

#include "raft/types.h"

namespace raft {

// Stores the latest request id and its result for each client. Request ids are
// expected to be monotonically increasing; only the latest id is retained.
class RequestResultStore {
public:
  virtual ~RequestResultStore() = default;

  virtual std::expected<std::optional<std::string>, Error>
  find(uint64_t client_id, uint64_t request_id) const = 0;

  virtual std::expected<void, Error>
  save(uint64_t client_id, uint64_t request_id,
       std::string result) = 0;
};

class MemoryRequestResultStore final : public RequestResultStore {
public:
  std::expected<std::optional<std::string>, Error>
  find(uint64_t client_id, uint64_t request_id) const override {
    const auto it = entries_.find(client_id);
    if (it == entries_.end()) {
      return std::optional<std::string>{};
    }
    if (it->second.request_id == request_id) {
      return it->second.result;
    }
    if (it->second.request_id > request_id) {
      return std::unexpected(Error{
          ErrorCode::InvalidArgument,
          "request id is older than the client's latest applied request"});
    }
    return std::optional<std::string>{};
  }

  std::expected<void, Error>
  save(uint64_t client_id, uint64_t request_id,
       std::string result) override {
    entries_[client_id] = Entry{request_id, std::move(result)};
    return {};
  }

private:
  struct Entry {
    uint64_t request_id = 0;
    std::string result;
  };

  std::map<uint64_t, Entry> entries_;
};

} // namespace raft
