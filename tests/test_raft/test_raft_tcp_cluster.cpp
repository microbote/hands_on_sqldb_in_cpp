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
                  connector) {
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
        std::move(options), [this](std::function<void()> work) {
          return runtime_->post(std::move(work));
        });
    node_ = std::make_unique<raft::RaftNode>(
        raft::NodeConfig{id, peers, 200, 40}, log_, *transport_,
        *state_machine_, clock);
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

} // namespace
