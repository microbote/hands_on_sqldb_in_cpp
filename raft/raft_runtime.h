#pragma once

#include <atomic>
#include <cstddef>
#include <expected>
#include <functional>
#include <string>
#include <thread>

#include "common/svrkit/service.h"
#include "raft/raft_executor.h"
#include "raft/raft_node.h"

namespace raft {

// Owns the single thread that is allowed to touch a RaftNode.
//
// Everything that mutates Raft state (propose, read_barrier, tick, inbound
// messages) runs on one service thread, so RaftNode itself needs no locks.
// Other threads submit work through this class:
//
//   - propose()/read_barrier()/run() are blocking submissions: the work runs
//     on the service thread, the caller waits for it to finish and then (for
//     proposals) waits on the returned completion. The service thread is never
//     blocked by a caller.
//   - post_message()/request_tick() are fire-and-forget: the transport thread
//     and the timer thread must never call RaftNode directly.
//
// Calling a blocking submission from the service thread itself returns
// ErrorCode::Busy instead of deadlocking.
//
// It is also the production RaftExecutor: the SQL adapter submits proposals and
// read barriers through this class, never through RaftNode.
class RaftRuntime : public RaftExecutor {
public:
  explicit RaftRuntime(RaftNode &node, std::string name = "raft");
  ~RaftRuntime();

  RaftRuntime(const RaftRuntime &) = delete;
  RaftRuntime &operator=(const RaftRuntime &) = delete;

  // Starts the service thread and waits until it is ready to accept work.
  void start(size_t queue_max = 4096);
  // Stops accepting work, drains whatever is queued, and joins the thread.
  void stop();
  bool running() const { return running_.load(); }

  // Runs `fn` on the service thread and waits for it to finish. Thread-safe.
  std::expected<void, Error> run(std::function<void()> fn);

  // Fire-and-forget, thread-safe. `false` = dropped (stopping or queue full).
  // The transport uses this to hand received messages to the service thread.
  bool post(std::function<void()> work);

  // Blocking submissions. The returned Proposal/ReadIndex still has to be
  // waited on by the caller (they complete when the entry commits/applies,
  // which happens on the service thread).
  std::expected<Proposal, Error> propose(std::string data) override;
  std::expected<ReadIndex, Error> read_barrier() override;
  NodeId node_id() const override { return node_.node_id(); }
  uint64_t election_timeout_ms() const override;
  std::optional<NodeId> leader_hint() override;

  // Thread-safe, non-blocking. `false` = dropped because the service is
  // stopping or its queue is full.
  bool post_message(NodeId from, const Message &message);
  bool request_tick();

  size_t queue_size() const { return service_.queue_size(); }
  uint64_t rejected_submissions() const { return rejected_.load(); }
  uint64_t dropped_ticks() const { return dropped_ticks_.load(); }
  const std::string &name() const { return service_.name(); }
  bool on_service_thread() const {
    return std::this_thread::get_id() == worker_id_.load();
  }

private:
  bool ready() const { return running_.load(); }
  bool submit_blocking(std::function<void()> work, Error *error);

  RaftNode &node_;
  common::svrkit::ServiceThread service_;
  std::atomic<std::thread::id> worker_id_{};
  std::atomic<bool> running_{false};
  std::atomic<uint64_t> rejected_{0};
  std::atomic<uint64_t> dropped_ticks_{0};
};

} // namespace raft
