#pragma once

#include <memory>
#include <string>

#include "raft/request_result_store.h"
#include "raft/state_machine.h"
#include "storage/kv_engine/kv_engine.h"

namespace raft {

// Applies a decoded kv::WriteBatch to a local KVStore.
//
// The local store is the Raft state machine. write_batch() and
// new_iterator() are the raw storage operations; session TxBuffer and the
// process-wide write slot are intentionally not involved.
class KVStateMachine final : public StateMachine {
public:
  KVStateMachine(std::shared_ptr<kv::KVStore> local,
                RequestResultStore &request_results);

  std::expected<std::string, Error>
  apply(const LogEntry &entry) override;

  std::expected<std::string, Error> snapshot(kv::KeyRange range) override;
  std::expected<void, Error> restore(std::string_view snapshot) override;
  std::expected<void, Error> restore(kv::KeyRange range,
                                    std::string_view snapshot) override;

private:
  std::shared_ptr<kv::KVStore> local_;
  RequestResultStore &request_results_;
};

} // namespace raft
