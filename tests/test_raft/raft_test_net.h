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

class TestTransport;

class TestNetwork {
public:
  struct QueuedMessage {
    NodeId from;
    NodeId to;
    uint64_t group_id = 0;
    raft::Message message;
  };

  // Defined after TestTransport so the dispatcher can call into it.
  void bind(NodeId id, TestTransport *transport);

  void enqueue(NodeId from, NodeId to, uint64_t group_id,
               raft::Message message) {
    messages_.push_back(
        QueuedMessage{from, to, group_id, std::move(message)});
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

  void release_held(size_t count = 1);
  void deliver_all(size_t limit = 10000);

private:
  std::map<NodeId, TestTransport *> transports_;
  std::deque<QueuedMessage> messages_;
  std::deque<QueuedMessage> held_;
  std::set<NodeId> isolated_;
  std::set<std::pair<NodeId, NodeId>> held_pairs_;
};

class TestTransport final : public raft::Transport {
public:
  TestTransport(NodeId sender, TestNetwork &network)
      : sender_(sender), network_(network) {}

  void send(NodeId to, uint64_t group_id,
            const raft::Message &message) override {
    network_.enqueue(sender_, to, group_id, message);
  }

  void on_message(
      uint64_t group_id,
      std::function<void(NodeId, const raft::Message &)> callback) override {
    on_messages_[group_id] = std::move(callback);
  }

  // Called by TestNetwork to deliver a queued message to this node's
  // transport. Delivers only to the callback registered for `group_id`, so
  // messages of one group never reach another group's RaftNode.
  void dispatch(uint64_t group_id, NodeId from, const raft::Message &message) {
    const auto it = on_messages_.find(group_id);
    if (it == on_messages_.end()) {
      return;
    }
    it->second(from, message);
  }

private:
  NodeId sender_;
  TestNetwork &network_;
  std::map<uint64_t, std::function<void(NodeId, const raft::Message &)>>
      on_messages_;
};

inline void TestNetwork::bind(NodeId id, TestTransport *transport) {
  transports_[id] = transport;
}

inline void TestNetwork::release_held(size_t count) {
  while (!held_.empty() && count-- > 0) {
    QueuedMessage item = std::move(held_.front());
    held_.pop_front();
    const auto it = transports_.find(item.to);
    if (it != transports_.end()) {
      it->second->dispatch(item.group_id, item.from, item.message);
    }
  }
}

inline void TestNetwork::deliver_all(size_t limit) {
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
    const auto it = transports_.find(item.to);
    if (it != transports_.end()) {
      it->second->dispatch(item.group_id, item.from, item.message);
    }
  }
  CHECK_TRUE(messages_.empty());
}

} // namespace raft_test
