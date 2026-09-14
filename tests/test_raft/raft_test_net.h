#pragma once

// Deterministic in-proc Raft test double: no sockets, no real time.
//
// Transport::send() only enqueues; delivery happens when the test calls
// deliver_all(). This mirrors the production rule that network callbacks must
// never re-enter RaftNode synchronously.

#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <set>
#include <utility>

#include "raft/clock.h"
#include "raft/raft_node.h"
#include "raft/transport.h"
#include "raft/types.h"
#include "test_framework.h"

namespace raft_test {

using raft::NodeId;

class ManualClock final : public raft::Clock {
public:
  uint64_t now_ms() const override { return now_; }
  void advance(uint64_t ms) { now_ += ms; }

private:
  uint64_t now_ = 0;
};

class TestNetwork {
public:
  struct QueuedMessage {
    NodeId from;
    NodeId to;
    raft::Message message;
  };

  void bind(NodeId id, raft::RaftNode *node) { nodes_[id] = node; }

  void enqueue(NodeId from, NodeId to, raft::Message message) {
    messages_.push_back(QueuedMessage{from, to, std::move(message)});
  }

  // Simulates a network partition: every message to or from this node is
  // dropped until unblock() is called.
  void block(NodeId id) { isolated_.insert(id); }
  void unblock(NodeId id) { isolated_.erase(id); }

  // Simulates a delayed link: messages matching this direction are parked
  // instead of delivered until release_held() is called. Tests use it to
  // replay acknowledgements out of order.
  void hold(NodeId from, NodeId to) { held_pairs_.insert({from, to}); }

  size_t held_count() const { return held_.size(); }

  void release_held(size_t count = 1) {
    while (!held_.empty() && count-- > 0) {
      QueuedMessage item = std::move(held_.front());
      held_.pop_front();
      const auto it = nodes_.find(item.to);
      if (it != nodes_.end()) {
        it->second->handle_message(item.from, item.message);
      }
    }
  }

  void deliver_all(size_t limit = 10000) {
    while (!messages_.empty() && limit-- > 0) {
      const QueuedMessage item = std::move(messages_.front());
      messages_.pop_front();
      if (isolated_.contains(item.from) || isolated_.contains(item.to)) {
        continue;
      }
      if (held_pairs_.contains({item.from, item.to})) {
        held_.push_back(item);
        continue;
      }
      const auto it = nodes_.find(item.to);
      if (it != nodes_.end()) {
        it->second->handle_message(item.from, item.message);
      }
    }
    CHECK_TRUE(messages_.empty());
  }

private:
  std::map<NodeId, raft::RaftNode *> nodes_;
  std::deque<QueuedMessage> messages_;
  std::deque<QueuedMessage> held_;
  std::set<NodeId> isolated_;
  std::set<std::pair<NodeId, NodeId>> held_pairs_;
};

class TestTransport final : public raft::Transport {
public:
  TestTransport(NodeId sender, TestNetwork &network)
      : sender_(sender), network_(network) {}

  void send(NodeId to, const raft::Message &message) override {
    network_.enqueue(sender_, to, message);
  }

  void on_message(
      std::function<void(NodeId, const raft::Message &)>) override {
    // Tests deliver messages directly through TestNetwork. This keeps message
    // handling queued rather than re-entering RaftNode inside send().
  }

private:
  NodeId sender_;
  TestNetwork &network_;
};

} // namespace raft_test
