// P1 adapter tests: kv::KVStore/KVEngine on top of a single Raft group.
//
// These tests do not use SQL; they exercise the exact interface the session
// layer sees, driven by the deterministic in-proc network and fake clock.

#include <chrono>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "raft/kv_state_machine.h"
#include "raft/memory_log_store.h"
#include "raft/proposal_payload.h"
#include "raft/raft_kv_store.h"
#include "raft/request_result_store.h"
#include "raft_test_net.h"
#include "storage/mock_engine/mock_engine.h"
#include "test_framework.h"

namespace {

using raft::NodeId;
using raft_test::ManualClock;
using raft_test::TestNetwork;
using raft_test::TestTransport;

class KVTestNode {
public:
  KVTestNode(NodeId id, std::vector<NodeId> peers, ManualClock &clock,
             TestNetwork &network, uint64_t election_timeout = 100,
             uint64_t heartbeat = 10,
             std::map<uint64_t, std::string> client_endpoints = {}) {
    local_ = std::make_shared<kv::MockStore>();
    state_machine_ =
        std::make_unique<raft::KVStateMachine>(local_, request_results_);
    transport_ = std::make_unique<TestTransport>(id, network);
    node_ = std::make_unique<raft::RaftNode>(
        raft::NodeConfig{id, std::move(peers), election_timeout, heartbeat},
        log_, *transport_, *state_machine_, clock);
    // Tests drive the node themselves, so they use the direct executor; the
    // production path goes through RaftRuntime (see server/raft_bootstrap).
    executor_ = std::make_unique<raft::RaftNodeExecutor>(*node_);
    store_ = std::make_shared<raft::RaftKVStore>(local_, *executor_,
                                                 std::move(client_endpoints));
    CHECK_EQ(store_->open(kv::DatabaseOptions{}), kv::Status::OK);
  }

  raft::RaftNode *node() { return node_.get(); }
  raft::RaftKVStore *store() { return store_.get(); }

  std::shared_ptr<kv::KVEngine> connect() { return store_->connect(); }

  kv::Status local_get(const kv::Key &key, kv::ByteValue *value) {
    return local_->get(key, value);
  }

private:
  std::shared_ptr<kv::MockStore> local_;
  raft::MemoryRequestResultStore request_results_;
  raft::MemoryLogStore log_;
  std::unique_ptr<raft::KVStateMachine> state_machine_;
  std::unique_ptr<TestTransport> transport_;
  std::unique_ptr<raft::RaftNode> node_;
  std::unique_ptr<raft::RaftNodeExecutor> executor_;
  std::shared_ptr<raft::RaftKVStore> store_;
};

class KVTestCluster {
public:
  explicit KVTestCluster(std::vector<NodeId> ids, uint64_t election_timeout = 100,
                         uint64_t heartbeat = 10,
                         std::map<uint64_t, std::string> client_endpoints = {})
      : ids_(std::move(ids)), election_timeout_(election_timeout),
        heartbeat_(heartbeat), client_endpoints_(std::move(client_endpoints)) {
    for (const NodeId id : ids_) {
      auto node = std::make_unique<KVTestNode>(id, ids_, clock_, network_,
                                              election_timeout_, heartbeat_,
                                              client_endpoints_);
      network_.bind(id, node->node());
      nodes_[id] = std::move(node);
    }
  }

  void start() {
    for (auto &[id, node] : nodes_) {
      CHECK_TRUE(node->node()->start().has_value());
    }
  }

  void step(uint64_t ms = 10) {
    clock_.advance(ms);
    for (auto &[id, node] : nodes_) {
      node->node()->tick();
    }
    network_.deliver_all();
  }

  // Drives the cluster until a leader exists and its first no-op is applied.
  void elect_leader() {
    for (int i = 0; i < 50 && leader() == nullptr; ++i) {
      step();
    }
    CHECK_NOT_NULL(leader());
    for (int i = 0; i < 10; ++i) {
      step();
    }
  }

  void block(NodeId id) { network_.block(id); }

  void tick_one(NodeId id, uint64_t ms = 10) {
    clock_.advance(ms);
    nodes_.at(id)->node()->tick();
  }

  void deliver_messages() { network_.deliver_all(); }

  raft::RaftNode *leader() {
    raft::RaftNode *found = nullptr;
    for (auto &[id, node] : nodes_) {
      if (node->node()->is_leader()) {
        CHECK_TRUE(found == nullptr);
        found = node->node();
      }
    }
    return found;
  }

  KVTestNode &node(NodeId id) { return *nodes_.at(id); }

  NodeId follower_id() {
    for (auto &[id, node] : nodes_) {
      if (!node->node()->is_leader()) {
        return id;
      }
    }
    return NodeId{0};
  }

private:
  std::vector<NodeId> ids_;
  uint64_t election_timeout_;
  uint64_t heartbeat_;
  std::map<uint64_t, std::string> client_endpoints_;
  ManualClock clock_;
  TestNetwork network_;
  std::map<NodeId, std::unique_ptr<KVTestNode>> nodes_;
};

TEST(RaftKVAdapter, LeaderEngineReplicatesAndReadsOwnWrites) {
  KVTestCluster cluster({NodeId{1}});
  cluster.start();
  cluster.elect_leader();

  auto engine = cluster.node(NodeId{1}).connect();
  CHECK_TRUE(engine != nullptr);

  CHECK_EQ(engine->put("a", "1"), kv::Status::OK);
  CHECK_EQ(engine->put("b", "2"), kv::Status::OK);

  kv::ByteValue value;
  CHECK_EQ(engine->get("a", &value), kv::Status::OK);
  CHECK_STREQ(value, "1");
  CHECK_TRUE(engine->exists("b"));

  CHECK_EQ(engine->remove("a"), kv::Status::OK);
  CHECK_EQ(engine->get("a", &value), kv::Status::NotFound);

  // The same bytes are visible in the replicated state machine's local store,
  // which is what followers receive through the log.
  CHECK_EQ(cluster.node(NodeId{1}).local_get("b", &value), kv::Status::OK);
  CHECK_STREQ(value, "2");
}

TEST(RaftKVAdapter, ExplicitTransactionSeesOwnWritesAndCommitsAtomically) {
  KVTestCluster cluster({NodeId{1}});
  cluster.start();
  cluster.elect_leader();

  auto engine = cluster.node(NodeId{1}).connect();
  CHECK_EQ(engine->put("kept", "old"), kv::Status::OK);

  CHECK_EQ(engine->begin_transaction(), kv::Status::OK);
  CHECK_TRUE(engine->in_transaction());
  CHECK_EQ(engine->put("kept", "new"), kv::Status::OK);
  CHECK_EQ(engine->put("fresh", "1"), kv::Status::OK);
  CHECK_EQ(engine->remove("kept-and-gone"), kv::Status::OK);

  kv::ByteValue value;
  CHECK_EQ(engine->get("kept", &value), kv::Status::OK);
  CHECK_STREQ(value, "new");

  CHECK_EQ(engine->commit_transaction(), kv::Status::OK);
  CHECK_FALSE(engine->in_transaction());

  // A second connection only sees the batch after it committed through Raft.
  auto other = cluster.node(NodeId{1}).connect();
  CHECK_EQ(other->get("kept", &value), kv::Status::OK);
  CHECK_STREQ(value, "new");
  CHECK_EQ(other->get("fresh", &value), kv::Status::OK);
  CHECK_STREQ(value, "1");
}

TEST(RaftKVAdapter, RollbackDiscardsBufferedWrites) {
  KVTestCluster cluster({NodeId{1}});
  cluster.start();
  cluster.elect_leader();

  auto engine = cluster.node(NodeId{1}).connect();
  CHECK_EQ(engine->begin_transaction(), kv::Status::OK);
  CHECK_EQ(engine->put("tmp", "x"), kv::Status::OK);

  kv::ByteValue value;
  CHECK_EQ(engine->get("tmp", &value), kv::Status::OK);
  CHECK_STREQ(value, "x");

  CHECK_EQ(engine->rollback_transaction(), kv::Status::OK);
  CHECK_EQ(engine->get("tmp", &value), kv::Status::NotFound);
  CHECK_EQ(cluster.node(NodeId{1}).local_get("tmp", &value),
           kv::Status::NotFound);
}

TEST(RaftKVAdapter, TransactionHoldsTheGroupWriteSlot) {
  KVTestCluster cluster({NodeId{1}});
  cluster.start();
  cluster.elect_leader();

  auto writer = cluster.node(NodeId{1}).connect();
  auto other = cluster.node(NodeId{1}).connect();

  CHECK_EQ(writer->begin_transaction(), kv::Status::OK);
  CHECK_EQ(writer->put("k", "1"), kv::Status::OK);

  // Pessimistic single writer, now scoped to the group instead of the process.
  CHECK_EQ(other->put("k", "2"), kv::Status::Busy);

  CHECK_EQ(writer->commit_transaction(), kv::Status::OK);
  CHECK_EQ(other->put("k", "2"), kv::Status::OK);

  kv::ByteValue value;
  CHECK_EQ(other->get("k", &value), kv::Status::OK);
  CHECK_STREQ(value, "2");
}

TEST(RaftKVAdapter, FollowerEngineReturnsNotLeader) {
  KVTestCluster cluster({NodeId{1}, NodeId{2}, NodeId{3}});
  cluster.start();
  cluster.elect_leader();

  auto follower = cluster.node(cluster.follower_id()).connect();
  CHECK_TRUE(follower != nullptr);

  CHECK_EQ(follower->put("k", "v"), kv::Status::NotLeader);
  CHECK_EQ(follower->last_error(), kv::Status::NotLeader);
  kv::ByteValue value;
  CHECK_EQ(follower->get("k", &value), kv::Status::NotLeader);
  CHECK_EQ(follower->last_error(), kv::Status::NotLeader);
}

TEST(RaftKVAdapter, LeaderHintPointsAtTheLeaderClientEndpoint) {
  std::map<uint64_t, std::string> endpoints{{1, "10.0.0.1:5433"},
                                            {2, "10.0.0.2:5433"},
                                            {3, "10.0.0.3:5433"}};
  KVTestCluster cluster({NodeId{1}, NodeId{2}, NodeId{3}}, 100, 10, endpoints);
  cluster.start();
  cluster.elect_leader();

  raft::RaftNode *leader = cluster.leader();
  CHECK_NOT_NULL(leader);
  const NodeId leader_id = leader->node_id();

  // The leader itself never redirects.
  CHECK_FALSE(cluster.node(leader_id).store()->leader_hint().has_value());

  // Followers point at the leader, with the client-facing SQL endpoint.
  for (const NodeId id : std::vector<NodeId>{NodeId{1}, NodeId{2}, NodeId{3}}) {
    if (id == leader_id) {
      continue;
    }
    const auto hint = cluster.node(id).store()->leader_hint();
    CHECK_TRUE(hint.has_value());
    if (hint.has_value()) {
      CHECK_EQ(hint->node_id, leader_id.value);
      CHECK_EQ(hint->endpoint, endpoints[leader_id.value]);
    }
  }
}

TEST(RaftKVAdapter, LeaderWithUnknownClientAddressOmitsTheEndpoint) {
  // Endpoint table without an entry for node 2, which we make the leader.
  std::map<uint64_t, std::string> endpoints{{1, "10.0.0.1:5433"}};
  KVTestCluster cluster({NodeId{1}, NodeId{2}}, 100, 10, endpoints);
  cluster.start();
  cluster.tick_one(NodeId{2}, 200); // only node 2 campaigns
  cluster.deliver_messages();
  CHECK_TRUE(cluster.node(NodeId{2}).node()->is_leader());

  const auto hint = cluster.node(NodeId{1}).store()->leader_hint();
  CHECK_TRUE(hint.has_value());
  if (hint.has_value()) {
    CHECK_EQ(hint->node_id, uint64_t{2});
    CHECK_TRUE(hint->endpoint.empty());
  }
}

TEST(RaftKVAdapter, MultiNodeReplicationAppliesToEveryLocalStore) {
  KVTestCluster cluster({NodeId{1}, NodeId{2}, NodeId{3}});
  cluster.start();
  cluster.elect_leader();

  raft::ProposalPayload payload;
  payload.client_id = 42;
  payload.request_id = 1;
  payload.batch.put("replicated", "value");

  // propose() (unlike the engine's blocking write) does not wait for commit,
  // so the single test thread can keep driving the cluster.
  auto proposal =
      cluster.leader()->propose(raft::encode_proposal_payload(payload));
  CHECK_TRUE(proposal.has_value());

  for (int i = 0; i < 10; ++i) {
    cluster.step();
  }
  auto committed = proposal->wait_for(std::chrono::milliseconds{100});
  CHECK_TRUE(committed.has_value());

  for (const NodeId id : std::vector<NodeId>{NodeId{1}, NodeId{2}, NodeId{3}}) {
    kv::ByteValue value;
    CHECK_EQ(cluster.node(id).local_get("replicated", &value), kv::Status::OK);
    CHECK_STREQ(value, "value");
    CHECK_EQ(cluster.node(id).node()->applied_index(),
             proposal->index);
  }
}

TEST(RaftKVAdapter, IsolatedLeaderFailsReadsAndWritesWithTimeout) {
  KVTestCluster cluster({NodeId{1}, NodeId{2}, NodeId{3}}, 40, 5);
  cluster.start();
  cluster.elect_leader();

  raft::RaftNode *leader = cluster.leader();
  CHECK_NOT_NULL(leader);
  auto engine = cluster.node(leader->node_id()).connect();

  cluster.block(leader->node_id());

  kv::ByteValue value;
  CHECK_EQ(engine->get("k", &value), kv::Status::Timeout);
  // A timed-out proposal has an unknown outcome: the entry may still commit
  // later, which is why retries must reuse the same client request id.
  CHECK_EQ(engine->put("k", "v"), kv::Status::Timeout);
}

TEST(RaftKVAdapter, RawStoreWritePathIsNotSupported) {
  KVTestCluster cluster({NodeId{1}});
  cluster.start();
  cluster.elect_leader();

  kv::WriteBatch batch;
  batch.put("k", "v");
  CHECK_EQ(cluster.node(NodeId{1}).store()->write_batch(batch),
           kv::Status::NotSupported);
}

} // namespace
