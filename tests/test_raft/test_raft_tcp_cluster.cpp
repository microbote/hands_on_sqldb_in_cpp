// Three real Raft nodes over real TCP transports, wired with socketpairs
// instead of listeners (the sandbox denies bind()).
//
// Covered end to end: the TCP transport's handshake + framing, the sender and
// inbound threads, RaftRuntime's service thread, election over the wire and
// replication into every node's state machine.

#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <expected>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "raft/kv_state_machine.h"
#include "raft/memory_log_store.h"
#include "raft/proposal_payload.h"
#include "raft/raft_node.h"
#include "raft/raft_runtime.h"
#include "raft/request_result_store.h"
#include "raft/tcp_transport.h"
#include "storage/mock_engine/mock_engine.h"
#include "test_framework.h"

namespace {

using raft::NodeId;

int64_t now_ms() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

// The RaftNode methods run on the service threads, so the clock must tolerate
// the test thread advancing it concurrently.
class AtomicManualClock final : public raft::Clock {
public:
  uint64_t now_ms() const override { return now_.load(); }
  void advance(uint64_t ms) { now_.fetch_add(ms); }

private:
  std::atomic<uint64_t> now_{0};
};

class ClusterNode {
public:
  ClusterNode(NodeId id, const std::vector<NodeId> &peers,
              AtomicManualClock &clock,
              std::function<std::expected<int, std::string>(const raft::PeerConfig &)>
                  connector,
              uint64_t snapshot_entries = 0) {
    local_ = std::make_shared<kv::MockStore>();
    CHECK_EQ(local_->open(kv::DatabaseOptions{}), kv::Status::OK);
    state_machine_ =
        std::make_unique<raft::KVStateMachine>(local_, request_results_);

    raft::RaftTcpTransport::Options options;
    options.node_id = id;
    options.logger = [](const char *, const std::string &) {};
    for (const NodeId peer : peers) {
      options.peers.push_back(
          raft::PeerConfig{peer, "127.0.0.1", static_cast<uint16_t>(5000 + peer.value)});
    }
    options.connect = std::move(connector);
    options.reconnect_backoff_ms = 20;
    transport_ = std::make_unique<raft::RaftTcpTransport>(
        std::move(options),
        [this](uint64_t /*group_id*/, std::function<void()> work) {
          return runtime_->post(std::move(work));
        });
    raft::NodeConfig node_config{id, peers, 200, 40};
    node_config.group_range = kv::KeyRange::from(kv::Key{});
    node_config.snapshot_entries_threshold = snapshot_entries;
    node_ = std::make_unique<raft::RaftNode>(
        std::move(node_config), log_, *transport_, *state_machine_, clock);
    runtime_ = std::make_unique<raft::RaftRuntime>(*node_, "raft-cluster");
    runtime_->start();
    std::expected<void, raft::Error> started;
    auto ran = runtime_->run([&] { started = node_->start(); });
    CHECK_TRUE(ran.has_value());
    CHECK_TRUE(started.has_value());
  }

  ~ClusterNode() {
    transport_->stop();
    if (inbound_.joinable()) {
      inbound_.join();
    }
    runtime_->stop();
  }

  void attach_inbound(int fd) { transport_->attach_inbound_connection(fd); }

  void start_inbound_loop() {
    inbound_ = std::thread([this] { transport_->run_inbound(); });
  }

  raft::RaftRuntime &runtime() { return *runtime_; }
  kv::MockStore &local() { return *local_; }

  bool is_leader() {
    bool leader = false;
    runtime_->run([&] { leader = node_->is_leader(); });
    return leader;
  }

  uint64_t last_included_index() {
    uint64_t value = 0;
    runtime_->run([&] { value = node_->last_included_index(); });
    return value;
  }

  uint64_t last_log_index() {
    uint64_t value = 0;
    runtime_->run([&] { value = node_->last_log_index(); });
    return value;
  }

  kv::Status applied_value(const kv::Key &key, kv::ByteValue *value) {
    return local_->get(key, value);
  }

private:
  std::shared_ptr<kv::MockStore> local_;
  raft::MemoryRequestResultStore request_results_;
  raft::MemoryLogStore log_;
  std::unique_ptr<raft::KVStateMachine> state_machine_;
  std::unique_ptr<raft::RaftTcpTransport> transport_;
  std::unique_ptr<raft::RaftNode> node_;
  std::unique_ptr<raft::RaftRuntime> runtime_;
  std::thread inbound_;
};

// One socketpair per direction of every pair: node i dials node j on its own
// socket, node j reads it from the other end.
struct Wire {
  int dialer_fd = -1;   // owned by the dialing transport (dup'ed per connect)
  int acceptor_fd = -1; // attached to the accepting transport
};

TEST(RaftTcpCluster, ThreeNodesElectAndReplicateOverRealSockets) {
  const std::vector<NodeId> ids{NodeId{1}, NodeId{2}, NodeId{3}};
  AtomicManualClock clock;

  // wires[(from,to)] = socketpair used when `from` dials `to`.
  std::map<std::pair<uint64_t, uint64_t>, Wire> wires;
  for (const NodeId from : ids) {
    for (const NodeId to : ids) {
      if (from == to) {
        continue;
      }
      int pair[2] = {-1, -1};
      CHECK_EQ(::socketpair(AF_UNIX, SOCK_STREAM, 0, pair), 0);
      Wire wire;
      wire.dialer_fd = pair[0];   // the transport dups this for its dialer
      wire.acceptor_fd = pair[1]; // goes to the accepting side
      wires[{from.value, to.value}] = wire;
    }
  }

  std::vector<std::unique_ptr<ClusterNode>> nodes;
  for (const NodeId id : ids) {
    auto connector = [&wires, id](const raft::PeerConfig &peer)
        -> std::expected<int, std::string> {
      const auto it = wires.find({id.value, peer.node_id.value});
      if (it == wires.end()) {
        return std::unexpected("no wire for this peer");
      }
      return ::dup(it->second.dialer_fd);
    };
    nodes.push_back(
        std::make_unique<ClusterNode>(id, ids, clock, std::move(connector)));
  }
  for (const NodeId id : ids) {
    for (const NodeId peer : ids) {
      if (peer == id) {
        continue;
      }
      nodes[id.value - 1]->attach_inbound(wires[{peer.value, id.value}].acceptor_fd);
    }
    nodes[id.value - 1]->start_inbound_loop();
  }

  // Drive the (injected) clock until exactly one node claims leadership.
  const int64_t election_deadline = now_ms() + 8000;
  ClusterNode *leader = nullptr;
  while (now_ms() < election_deadline) {
    clock.advance(20);
    for (auto &node : nodes) {
      node->runtime().request_tick();
    }
    std::this_thread::sleep_for(std::chrono::milliseconds{5});
    int leaders = 0;
    ClusterNode *candidate = nullptr;
    for (auto &node : nodes) {
      if (node->is_leader()) {
        ++leaders;
        candidate = node.get();
      }
    }
    if (leaders == 1) {
      leader = candidate;
      break;
    }
  }
  CHECK_TRUE(leader != nullptr);
  if (leader == nullptr) {
    return;
  }

  raft::ProposalPayload payload;
  payload.client_id = 7;
  payload.request_id = 1;
  payload.batch.put("replicated", "value");
  auto proposal = leader->runtime().propose(raft::encode_proposal_payload(payload));
  CHECK_TRUE(proposal.has_value());
  if (!proposal.has_value()) {
    return;
  }

  // Keep ticking (heartbeats) while the commit round trip happens over TCP.
  const int64_t commit_deadline = now_ms() + 8000;
  bool committed = false;
  std::string last_error;
  while (now_ms() < commit_deadline) {
    clock.advance(20);
    for (auto &node : nodes) {
      node->runtime().request_tick();
    }
    auto result = proposal->wait_for(std::chrono::milliseconds{0});
    if (result.has_value()) {
      committed = result->committed;
      break;
    }
    last_error = std::string{raft::error_code_to_string(result.error().code)} +
                 ": " + result.error().message;
    std::this_thread::sleep_for(std::chrono::milliseconds{5});
  }
  if (!committed) {
    fmt::print(stderr, "commit did not finish, last status: {}\n", last_error);
  }
  CHECK_TRUE(committed);

  // Every replica's state machine must have applied the batch.
  bool all_replicas = false;
  const int64_t replica_deadline = now_ms() + 5000;
  while (now_ms() < replica_deadline) {
    all_replicas = true;
    for (auto &node : nodes) {
      kv::ByteValue value;
      if (node->applied_value("replicated", &value) != kv::Status::OK ||
          value != "value") {
        all_replicas = false;
        break;
      }
    }
    if (all_replicas) {
      break;
    }
    clock.advance(20);
    for (auto &node : nodes) {
      node->runtime().request_tick();
    }
    std::this_thread::sleep_for(std::chrono::milliseconds{10});
  }
  CHECK_TRUE(all_replicas);

  for (auto &node : nodes) {
    node.reset();
  }
  for (auto &[key, wire] : wires) {
    ::close(wire.dialer_fd);
  }
}

TEST(RaftTcpCluster, LateFollowerCatchesUpViaSnapshotOverRealSockets) {
  const std::vector<NodeId> ids{NodeId{1}, NodeId{2}, NodeId{3}, NodeId{4}};
  AtomicManualClock clock;

  // wires[(from,to)] = socketpair used when `from` dials `to`.
  std::map<std::pair<uint64_t, uint64_t>, Wire> wires;
  for (const NodeId from : ids) {
    for (const NodeId to : ids) {
      if (from == to) {
        continue;
      }
      int pair[2] = {-1, -1};
      CHECK_EQ(::socketpair(AF_UNIX, SOCK_STREAM, 0, pair), 0);
      wires[{from.value, to.value}] = Wire{pair[0], pair[1]};
    }
  }

  auto make_connector = [&wires](NodeId id) {
    return [&wires, id](const raft::PeerConfig &peer)
        -> std::expected<int, std::string> {
      const auto it = wires.find({id.value, peer.node_id.value});
      if (it == wires.end()) {
        return std::unexpected("no wire for this peer");
      }
      return ::dup(it->second.dialer_fd);
    };
  };

  const std::vector<NodeId> first_three{NodeId{1}, NodeId{2}, NodeId{3}};
  std::vector<std::unique_ptr<ClusterNode>> nodes;
  for (const NodeId id : first_three) {
    nodes.push_back(std::make_unique<ClusterNode>(
        id, ids, clock, make_connector(id), /*snapshot_entries=*/3));
  }
  // Node 4 is created but its inbound side stays unattached until after the
  // leader has compacted, so it is genuinely behind and must catch up via a
  // snapshot transfer over real sockets.
  auto node4 = std::make_unique<ClusterNode>(
      NodeId{4}, ids, clock, make_connector(NodeId{4}), /*snapshot_entries=*/3);

  for (const NodeId id : first_three) {
    for (const NodeId peer : ids) {
      if (peer == id) {
        continue;
      }
      nodes[id.value - 1]->attach_inbound(
          wires[{peer.value, id.value}].acceptor_fd);
    }
    nodes[id.value - 1]->start_inbound_loop();
  }

  std::vector<ClusterNode *> live{nodes[0].get(), nodes[1].get(),
                                  nodes[2].get()};
  auto tick_live = [&] {
    clock.advance(20);
    for (ClusterNode *node : live) {
      node->runtime().request_tick();
    }
  };

  // Nodes 1..3 form a quorum (3 of 4) and elect a leader without node 4.
  ClusterNode *leader = nullptr;
  const int64_t election_deadline = now_ms() + 8000;
  while (now_ms() < election_deadline) {
    tick_live();
    std::this_thread::sleep_for(std::chrono::milliseconds{5});
    int leaders = 0;
    ClusterNode *candidate = nullptr;
    for (auto &node : nodes) {
      if (node->is_leader()) {
        ++leaders;
        candidate = node.get();
      }
    }
    if (leaders == 1) {
      leader = candidate;
      break;
    }
  }
  CHECK_TRUE(leader != nullptr);
  if (leader == nullptr) {
    for (auto &node : nodes) {
      node.reset();
    }
    node4.reset();
    for (auto &[key, wire] : wires) {
      ::close(wire.dialer_fd);
    }
    return;
  }

  auto propose = [&](uint64_t request_id, const std::string &key) -> bool {
    raft::ProposalPayload payload;
    payload.client_id = 7;
    payload.request_id = request_id;
    payload.batch.put(key, "value-" + std::to_string(request_id));
    auto proposal =
        leader->runtime().propose(raft::encode_proposal_payload(payload));
    CHECK_TRUE(proposal.has_value());
    if (!proposal.has_value()) {
      return false;
    }
    const int64_t deadline = now_ms() + 8000;
    while (now_ms() < deadline) {
      tick_live();
      auto result = proposal->wait_for(std::chrono::milliseconds{0});
      if (result.has_value() && result->committed) {
        return true;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds{5});
    }
    return false;
  };

  for (uint64_t i = 1; i <= 4; ++i) {
    CHECK_TRUE(propose(i, "key-" + std::to_string(i)));
  }

  // Let the leader compact its log at the applied index.
  const int64_t compact_deadline = now_ms() + 8000;
  while (now_ms() < compact_deadline &&
         leader->last_included_index() < 3) {
    tick_live();
    std::this_thread::sleep_for(std::chrono::milliseconds{5});
  }
  CHECK_GE(leader->last_included_index(), 3);

  // Node 4 joins: attach its inbound connections and start its loop. The
  // peers' outbound connections to node 4 were established at construction,
  // so their frames are waiting in the socket buffers.
  for (const NodeId peer : first_three) {
    node4->attach_inbound(wires[{peer.value, NodeId{4}.value}].acceptor_fd);
  }
  node4->start_inbound_loop();
  for (const NodeId id : first_three) {
    nodes[id.value - 1]->attach_inbound(
        wires[{NodeId{4}.value, id.value}].acceptor_fd);
  }
  live.push_back(node4.get());

  // Node 4 must catch up via the snapshot: every pre-compaction key lands.
  const int64_t catchup_deadline = now_ms() + 15000;
  bool caught_up = false;
  while (now_ms() < catchup_deadline) {
    tick_live();
    std::this_thread::sleep_for(std::chrono::milliseconds{5});
    bool all = true;
    for (uint64_t i = 1; i <= 4; ++i) {
      kv::ByteValue value;
      if (node4->applied_value("key-" + std::to_string(i), &value) !=
          kv::Status::OK) {
        all = false;
        break;
      }
    }
    if (all) {
      caught_up = true;
      break;
    }
  }
  CHECK_TRUE(caught_up);
  CHECK_GE(node4->last_included_index(), 3);

  // A post-snapshot write replicates to all four nodes.
  CHECK_TRUE(propose(5, "key-5"));
  bool all_four = false;
  const int64_t all_deadline = now_ms() + 8000;
  while (now_ms() < all_deadline) {
    all_four = true;
    for (auto &node : nodes) {
      kv::ByteValue value;
      if (node->applied_value("key-5", &value) != kv::Status::OK) {
        all_four = false;
        break;
      }
    }
    if (all_four) {
      kv::ByteValue value;
      if (node4->applied_value("key-5", &value) != kv::Status::OK) {
        all_four = false;
      }
    }
    if (all_four) {
      break;
    }
    tick_live();
    std::this_thread::sleep_for(std::chrono::milliseconds{5});
  }
  CHECK_TRUE(all_four);

  node4.reset();
  for (auto &node : nodes) {
    node.reset();
  }
  for (auto &[key, wire] : wires) {
    ::close(wire.dialer_fd);
  }
}

} // namespace
