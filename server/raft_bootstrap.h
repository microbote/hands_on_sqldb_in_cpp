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

// Assembles the raft-enabled startup path and owns its shutdown order.
//
// Startup (mirrors the lifecycle section of
// raft/codex_glm53_validate_design.md):
//
//   local KVStore (state machine storage, opened by the caller)
//     -> LevelDBLogStore            <log_path>/log
//     -> LevelDBRequestResultStore  <log_path>/request_results
//     -> KVStateMachine
//     -> RaftTcpTransport           (sender thread starts here)
//     -> RaftNode
//     -> RaftRuntime                (the only thread that touches RaftNode)
//     -> listen + inbound thread + heartbeat timer
//     -> RaftKVStore                (what the SQL server sees)
//
// Shutdown is the reverse: timer -> transport -> runtime -> stores.
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

  // Idempotent: stops the timer, the transport (sender + inbound), the raft
  // service thread and closes the raft stores.
  void stop();

  std::shared_ptr<kv::KVStore> store() const { return store_; }
  raft::RaftNode *node() const { return node_.get(); }
  raft::RaftTcpTransport *transport() const { return transport_.get(); }

private:
  RaftBootstrap() = default;

  bool post_to_runtime(std::function<void()> work);
  void on_tick();
  void arm_timer();

  Logger *logger_ = nullptr;
  std::shared_ptr<kv::KVStore> local_;
  raft::SystemClock clock_;
  raft::LevelDBLogStore log_store_;
  raft::LevelDBRequestResultStore request_results_;
  std::unique_ptr<raft::KVStateMachine> state_machine_;
  std::unique_ptr<raft::RaftTcpTransport> transport_;
  std::unique_ptr<raft::RaftNode> node_;
  std::unique_ptr<raft::RaftRuntime> runtime_;
  std::shared_ptr<raft::RaftKVStore> store_;
  common::svrkit::Loop timer_loop_{"raft-timer"};
  std::thread timer_thread_;
  std::thread inbound_thread_;
  int64_t heartbeat_ms_ = 100;
  bool stopped_ = false;
};

} // namespace server

#endif // SQLDB_HAVE_LEVELDB
