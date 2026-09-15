#pragma once

#if defined(SQLDB_HAVE_LEVELDB)

#include <cstdint>
#include <expected>
#include <functional>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "common/svrkit/loop.h"
#include "raft/group_router.h"
#include "raft/leveldb_log_store.h"
#include "raft/leveldb_request_result_store.h"
#include "raft/kv_state_machine.h"
#include "raft/raft_kv_store.h"
#include "raft/raft_node.h"
#include "raft/raft_runtime.h"
#include "raft/tcp_transport.h"
#include "server/config.h"
#include "server/logger.h"
#include "storage/kv_engine/kv_engine.h"

namespace server {

// One raft group's full stack: its own log/result stores, state machine
// (over the shared business KV), node and runtime. All groups of a node share
// a single RaftTcpTransport (multiplexed by group id) and one RaftKVStore.
struct RaftGroupState {
  raft::LevelDBLogStore log_store;
  raft::LevelDBRequestResultStore request_results;
  std::unique_ptr<raft::KVStateMachine> state_machine;
  std::unique_ptr<raft::RaftNode> node;
  std::unique_ptr<raft::RaftRuntime> runtime;
};

// Assembles the raft-enabled startup path and owns its shutdown order.
//
// Startup:
//   local KVStore (state machine storage, opened by the caller)
//     -> per group: LevelDBLogStore + LevelDBRequestResultStore + KVStateMachine
//     -> RaftTcpTransport (one, shared by all groups; sender thread starts here)
//     -> per group: RaftNode + RaftRuntime
//     -> listen + inbound thread + heartbeat timer (ticks every group)
//     -> RaftKVStore (what the SQL server sees)
//
// Single-group deployments use <log_path>/log and <log_path>/request_results
// exactly as before; multi-group uses <log_path>/group<N>/... per group.
//
// Shutdown is the reverse: timer -> transport -> runtimes -> stores.
class RaftBootstrap {
public:
  struct Options {
    // Test seam: skip binding the raft listener. Single-member groups skip it
    // anyway (nobody can connect), which also keeps them testable in sandboxes
    // where bind() is denied.
    bool bind_listener = true;
  };

  // Overload instead of a default argument: the nested Options type has a
  // default member initializer, which cannot be used in a default argument
  // inside the class definition.
  static std::expected<std::unique_ptr<RaftBootstrap>, std::string>
  open(const ServerConfig &config, std::shared_ptr<kv::KVStore> local,
       Logger &logger);
  static std::expected<std::unique_ptr<RaftBootstrap>, std::string>
  open(const ServerConfig &config, std::shared_ptr<kv::KVStore> local,
       Logger &logger, Options options);

  ~RaftBootstrap();

  RaftBootstrap(const RaftBootstrap &) = delete;
  RaftBootstrap &operator=(const RaftBootstrap &) = delete;

  // Idempotent: stops the timer, the transport (sender + inbound), every raft
  // service thread and closes every group's stores.
  void stop();

  std::shared_ptr<kv::KVStore> store() const { return store_; }
  raft::RaftNode *node() const { return node(0); }
  raft::RaftTcpTransport *transport() const { return transport_.get(); }
  size_t group_count() const { return groups_.size(); }
  raft::RaftNode *node(size_t group) const {
    return group < groups_.size() ? groups_[group]->node.get() : nullptr;
  }
  raft::RaftRuntime *runtime(size_t group) const {
    return group < groups_.size() ? groups_[group]->runtime.get() : nullptr;
  }

private:
  RaftBootstrap() = default;

  bool post_to_runtime(uint64_t group_id, std::function<void()> work);
  void on_tick();
  void arm_timer();

  Logger *logger_ = nullptr;
  std::shared_ptr<kv::KVStore> local_;
  raft::SystemClock clock_;
  std::vector<std::unique_ptr<RaftGroupState>> groups_;
  std::unique_ptr<raft::RaftTcpTransport> transport_;
  std::shared_ptr<raft::RaftKVStore> store_;
  common::svrkit::Loop timer_loop_{"raft-timer"};
  std::thread timer_thread_;
  std::thread inbound_thread_;
  int64_t heartbeat_ms_ = 100;
  bool stopped_ = false;
};

} // namespace server

#endif // SQLDB_HAVE_LEVELDB
