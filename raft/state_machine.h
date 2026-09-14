#pragma once

#include <expected>
#include <string_view>

#include "raft/types.h"
#include "storage/kv_engine/kv_engine.h"

namespace raft {

// The SQL-side implementation will decode a proposal payload and apply a
// kv::WriteBatch to the local KV store.
class StateMachine {
public:
  virtual ~StateMachine() = default;

  virtual std::expected<std::string, Error> apply(const LogEntry &entry) = 0;

  // Snapshot is part of the P0 contract. The initial tests focus on election,
  // replication, commit, and apply; transport of snapshots comes later.
  virtual std::expected<std::string, Error> snapshot(kv::KeyRange range) = 0;
  virtual std::expected<void, Error> restore(std::string_view snapshot) = 0;

  // Snapshot installation needs the exact group range so restore can clear
  // [start,end) before applying snapshot keys. Existing implementations that
  // only understand the design-doc signature can keep restore(string_view).
  virtual std::expected<void, Error>
  restore(kv::KeyRange range, std::string_view snapshot) {
    (void)range;
    return restore(snapshot);
  }
};

} // namespace raft
