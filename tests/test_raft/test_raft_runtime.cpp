// RaftRuntime: the single service thread that is allowed to touch RaftNode.
//
// The tests deliberately call the runtime from the test thread to prove that
// submissions are serialized onto the service thread, and that a blocking call
// made from that thread itself is rejected instead of deadlocking.

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <expected>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "raft/memory_log_store.h"
#include "raft/raft_runtime.h"
#include "raft_test_net.h"
#include "test_framework.h"

namespace {

using raft::NodeId;
using raft_test::ManualClock;
using raft_test::TestNetwork;
using raft_test::TestTransport;

class RecordingStateMachine final : public raft::StateMachine {
public:
  std::expected<std::string, raft::Error>
  apply(const raft::LogEntry &entry) override {
    applied.push_back(entry.data);
    return entry.data;
  }

  std::expected<std::string, raft::Error> snapshot(kv::KeyRange) override {
    return std::string{"snapshot"};
  }

  std::expected<void, raft::Error> restore(std::string_view) override {
    return {};
  }

  std::vector<std::string> applied;
};

class RuntimeFixture {
public:
  RuntimeFixture()
      : transport_(NodeId{1}, network_),
        node_(raft::NodeConfig{NodeId{1}, std::vector<NodeId>{NodeId{1}}, 100,
                               10},
              log_, transport_, state_machine_, clock_),
        runtime_(node_, "raft-test") {
    network_.bind(NodeId{1}, &node_);
    CHECK_TRUE(node_.start().has_value());
  }

  void start(size_t queue_max = 4096) { runtime_.start(queue_max); }

  // Single-node election: advance the injected clock and post one tick.
  void elect() {
    clock_.advance(200);
    CHECK_TRUE(runtime_.request_tick());
    CHECK_TRUE(barrier());
  }

  // request_tick()/post_message() are fire-and-forget; an empty run() is the
  // barrier that proves the service thread processed them. With a tiny
  // queue_max the submission can legitimately hit a still-busy queue, so retry
  // instead of treating that as a failure.
  bool barrier() {
    for (int attempt = 0; attempt < 100000; ++attempt) {
      if (runtime_.run([] {}).has_value()) {
        return true;
      }
      std::this_thread::yield();
    }
    return false;
  }

  raft::RaftRuntime &runtime() { return runtime_; }
  raft::RaftNode &node() { return node_; }

private:
  ManualClock clock_;
  TestNetwork network_;
  raft::MemoryLogStore log_;
  RecordingStateMachine state_machine_;
  TestTransport transport_;
  raft::RaftNode node_;
  raft::RaftRuntime runtime_;
};

TEST(RaftRuntime, ProposesOnTheServiceThreadAndCommits) {
  RuntimeFixture fixture;
  fixture.start();
  fixture.elect();

  auto proposal = fixture.runtime().propose("alpha");
  CHECK_TRUE(proposal.has_value());
  if (!proposal.has_value()) {
    return;
  }
  // A single-node group commits inside propose(), so the completion is already
  // finished when the call returns.
  auto completed = proposal->wait_for(std::chrono::milliseconds{100});
  CHECK_TRUE(completed.has_value());
  if (completed.has_value()) {
    CHECK_TRUE(completed->committed);
    CHECK_EQ(completed->index, uint64_t{2}); // 1 = election no-op
    CHECK_EQ(completed->apply_result, std::string("alpha"));
  }

  size_t applied = 0;
  CHECK_TRUE(fixture.runtime()
                 .run([&] { applied = fixture.node().applied_index(); })
                 .has_value());
  CHECK_EQ(applied, size_t{2});
}

TEST(RaftRuntime, RejectsBlockingCallsFromTheServiceThread) {
  RuntimeFixture fixture;
  fixture.start();
  fixture.elect();

  std::expected<raft::Proposal, raft::Error> inner =
      std::unexpected(raft::Error{raft::ErrorCode::Timeout, "not run yet"});
  bool invoked = false;
  auto outer = fixture.runtime().run([&] {
    invoked = true;
    inner = fixture.runtime().propose("would-deadlock");
  });
  CHECK_TRUE(outer.has_value());
  CHECK_TRUE(invoked);
  CHECK_FALSE(inner.has_value());
  if (!inner.has_value()) {
    CHECK_EQ(inner.error().code, raft::ErrorCode::Busy);
  }
}

TEST(RaftRuntime, DeliversMessagesPostedFromOtherThreads) {
  RuntimeFixture fixture;
  fixture.start();
  fixture.elect();

  uint64_t term_before = 0;
  CHECK_TRUE(fixture.runtime()
                 .run([&] { term_before = fixture.node().term(); })
                 .has_value());
  const uint64_t higher_term = term_before + 1;

  // The transport thread must never call handle_message() directly: it posts
  // the message and the service thread applies it.
  CHECK_TRUE(fixture.runtime().post_message(
      NodeId{2},
      raft::AppendEntriesRequest{higher_term, NodeId{2}, 0, 0, {}, 0}));

  uint64_t term_after = 0;
  raft::Role role = raft::Role::Candidate;
  CHECK_TRUE(fixture.runtime()
                 .run([&] {
                   term_after = fixture.node().term();
                   role = fixture.node().role();
                 })
                 .has_value());
  CHECK_EQ(term_after, higher_term);
  CHECK_TRUE(role != raft::Role::Leader);
}

TEST(RaftRuntime, ReportsQueueFullInsteadOfBlocking) {
  RuntimeFixture fixture;
  fixture.start(1 /* queue_max */);
  fixture.elect();

  // Occupy the service thread so it cannot drain the queue.
  std::mutex mutex;
  std::condition_variable condition;
  bool entered = false;
  bool release = false;
  bool blocking_call_ok = false;
  std::thread blocker([&] {
    blocking_call_ok = fixture.runtime()
                           .run([&] {
                             {
                               std::lock_guard<std::mutex> lock(mutex);
                               entered = true;
                             }
                             condition.notify_all();
                             std::unique_lock<std::mutex> lock(mutex);
                             condition.wait(lock, [&] { return release; });
                           })
                           .has_value();
  });
  {
    std::unique_lock<std::mutex> lock(mutex);
    condition.wait(lock, [&] { return entered; });
  }

  // The service thread is busy, so one queued item fills the single slot and
  // the next submission is dropped instead of blocking the caller.
  CHECK_TRUE(fixture.runtime().post_message(
      NodeId{2}, raft::RequestVoteRequest{1, NodeId{2}, 0, 0}));
  CHECK_FALSE(fixture.runtime().request_tick());
  CHECK_EQ(fixture.runtime().dropped_ticks(), uint64_t{1});

  {
    std::lock_guard<std::mutex> lock(mutex);
    release = true;
  }
  condition.notify_all();
  blocker.join();
  CHECK_TRUE(blocking_call_ok);

  // With the queue drained, submissions are accepted again.
  CHECK_TRUE(fixture.runtime().request_tick());
  CHECK_TRUE(fixture.barrier());
}

TEST(RaftRuntime, StopDrainsQueuedWork) {
  RuntimeFixture fixture;
  fixture.start();
  fixture.elect();

  uint64_t term_before = 0;
  CHECK_TRUE(fixture.runtime()
                 .run([&] { term_before = fixture.node().term(); })
                 .has_value());
  const uint64_t higher_term = term_before + 1;
  CHECK_TRUE(fixture.runtime().post_message(
      NodeId{2},
      raft::AppendEntriesRequest{higher_term, NodeId{2}, 0, 0, {}, 0}));

  fixture.runtime().stop();
  CHECK_FALSE(fixture.runtime().running());
  // stop() drains the queue and joins the thread, so reading node state here
  // is safe and the queued message must have been applied.
  CHECK_EQ(fixture.node().term(), higher_term);
}

TEST(RaftRuntime, RejectsCallsWhenNotRunning) {
  RuntimeFixture fixture; // intentionally not started
  auto proposal = fixture.runtime().propose("too-early");
  CHECK_FALSE(proposal.has_value());
  if (!proposal.has_value()) {
    CHECK_EQ(proposal.error().code, raft::ErrorCode::InternalError);
  }
  CHECK_FALSE(fixture.runtime().run([] {}).has_value());
  CHECK_FALSE(fixture.runtime().running());
}

} // namespace
