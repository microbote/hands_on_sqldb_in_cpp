#include <algorithm>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <string>
#include <vector>
#include <unistd.h>

#include "raft/kv_state_machine.h"
#include "raft/memory_log_store.h"
#include "raft/proposal_payload.h"
#include "raft/raft_node.h"
#include "raft_test_net.h"
#include "storage/kv_engine/kv_factory.h"
#include "test_framework.h"

#if defined(SQLDB_HAVE_LEVELDB)
#include "raft/leveldb_request_result_store.h"
#include "raft/leveldb_log_store.h"
#endif

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

  std::expected<std::string, raft::Error>
  snapshot(kv::KeyRange) override {
    return std::string{"snapshot"};
  }

  std::expected<void, raft::Error> restore(std::string_view) override {
    restored = true;
    return {};
  }

  bool contains(const std::string &data) const {
    return std::find(applied.begin(), applied.end(), data) != applied.end();
  }

  bool restored = false;
  std::vector<std::string> applied;
};

struct TestNode {
  raft::MemoryLogStore log;
  RecordingStateMachine state_machine;
  std::unique_ptr<TestTransport> transport;
  std::unique_ptr<raft::RaftNode> node;
};

class TestCluster {
public:
  TestCluster(std::vector<NodeId> ids, uint64_t election_timeout = 100,
              uint64_t heartbeat = 10)
      : ids_(std::move(ids)) {
    for (const NodeId id : ids_) {
      auto test_node = std::make_unique<TestNode>();
      test_node->transport = std::make_unique<TestTransport>(id, network_);
      test_node->node = std::make_unique<raft::RaftNode>(
          raft::NodeConfig{id, ids_, election_timeout, heartbeat},
          test_node->log, *test_node->transport,
          test_node->state_machine, clock_);
      network_.bind(id, test_node->node.get());
      nodes_[id] = std::move(test_node);
    }
  }

  void start() {
    for (auto &[id, test_node] : nodes_) {
      auto result = test_node->node->start();
      CHECK_TRUE(result.has_value());
    }
  }

  void step(uint64_t ms = 10) {
    clock_.advance(ms);
    for (auto &[id, test_node] : nodes_) {
      test_node->node->tick();
    }
    network_.deliver_all();
  }

  void tick_one(NodeId id, uint64_t ms = 10) {
    clock_.advance(ms);
    nodes_.at(id)->node->tick();
  }

  void deliver_messages() { network_.deliver_all(); }

  void block(NodeId id) { network_.block(id); }
  void unblock(NodeId id) { network_.unblock(id); }
  void hold(NodeId from, NodeId to) { network_.hold(from, to); }
  size_t held_count() const { return network_.held_count(); }
  void release_held(size_t count = 1) { network_.release_held(count); }

  raft::RaftNode *leader() const {
    raft::RaftNode *found = nullptr;
    for (const auto &[id, test_node] : nodes_) {
      if (test_node->node->is_leader()) {
        CHECK_TRUE(found == nullptr);
        found = test_node->node.get();
      }
    }
    return found;
  }

  raft::RaftNode *node(NodeId id) const {
    return nodes_.at(id)->node.get();
  }

  TestNode &test_node(NodeId id) { return *nodes_.at(id); }

private:
  std::vector<NodeId> ids_;
  ManualClock clock_;
  TestNetwork network_;
  std::map<NodeId, std::unique_ptr<TestNode>> nodes_;
};

#if defined(SQLDB_HAVE_LEVELDB)

class TempDirectory {
public:
  TempDirectory() {
    const auto base = std::filesystem::temp_directory_path() /
                      ("sqldb-raft-test-XXXXXX");
    // Keep one string: begin()/end() from two different temporaries would be
    // unrelated iterators (libc++ turns that into length_error("vector")).
    const std::string base_string = base.string();
    std::vector<char> buffer(base_string.begin(), base_string.end());
    buffer.push_back('\0');
    if (::mkdtemp(buffer.data()) == nullptr) {
      return;
    }
    path_ = buffer.data();
    valid_ = true;
  }

  ~TempDirectory() {
    if (valid_) {
      std::error_code ignored;
      std::filesystem::remove_all(path_, ignored);
    }
  }

  const std::string &path() const { return path_; }
  bool valid() const { return valid_; }

private:
  std::string path_;
  bool valid_ = false;
};

TEST(LevelDBLogStore, PersistsHardStateAndEntries) {
  TempDirectory directory;
  CHECK_TRUE(directory.valid());

  {
    raft::LevelDBLogStore store;
    CHECK_TRUE(store.open(directory.path()).has_value());

    const raft::HardState hard_state{7, NodeId{3}};
    CHECK_TRUE(store.save_hard_state(hard_state).has_value());
    CHECK_TRUE(store.append(raft::LogEntry{1, 5, "one"}).has_value());
    CHECK_TRUE(store.append(raft::LogEntry{
                   2, 7, std::string{"two\0with-nul", 12}})
                   .has_value());
    CHECK_EQ(store.last_index().value_or(0), uint64_t{2});
  }

  {
    raft::LevelDBLogStore store;
    CHECK_TRUE(store.open(directory.path()).has_value());

    auto hard_state = store.load_hard_state();
    CHECK_TRUE(hard_state.has_value());
    CHECK_EQ(hard_state->term, uint64_t{7});
    CHECK_EQ(hard_state->voted_for, NodeId{3});

    auto first = store.at(1);
    CHECK_TRUE(first.has_value());
    CHECK_EQ(first->term, uint64_t{5});
    CHECK_STREQ(first->data, "one");

    auto second = store.at(2);
    CHECK_TRUE(second.has_value());
    CHECK_EQ(second->term, uint64_t{7});
    const std::string expected_data{"two\0with-nul", 12};
    CHECK_EQ(second->data, expected_data);
    CHECK_EQ(store.last_index().value_or(0), uint64_t{2});
  }
}

TEST(LevelDBLogStore, TruncatesSuffixDurably) {
  TempDirectory directory;
  CHECK_TRUE(directory.valid());

  {
    raft::LevelDBLogStore store;
    CHECK_TRUE(store.open(directory.path()).has_value());
    CHECK_TRUE(store.append(raft::LogEntry{1, 1, "one"}).has_value());
    CHECK_TRUE(store.append(raft::LogEntry{2, 1, "two"}).has_value());
    CHECK_TRUE(store.append(raft::LogEntry{3, 2, "three"}).has_value());
    CHECK_TRUE(store.truncate_suffix(2).has_value());
    CHECK_EQ(store.last_index().value_or(99), uint64_t{1});
    CHECK_FALSE(store.at(2).has_value());
  }

  {
    raft::LevelDBLogStore store;
    CHECK_TRUE(store.open(directory.path()).has_value());
    CHECK_EQ(store.last_index().value_or(99), uint64_t{1});
    auto first = store.at(1);
    CHECK_TRUE(first.has_value());
    CHECK_STREQ(first->data, "one");
    CHECK_FALSE(store.at(2).has_value());
    CHECK_TRUE(store.append(raft::LogEntry{2, 3, "replacement"})
                   .has_value());
  }
}

TEST(LevelDBLogStore, InstallsSnapshotAndRecoversAfterRestart) {
  TempDirectory directory;
  CHECK_TRUE(directory.valid());

  {
    raft::LevelDBLogStore store;
    CHECK_TRUE(store.open(directory.path()).has_value());
    for (uint64_t i = 1; i <= 5; ++i) {
      CHECK_TRUE(store.append(raft::LogEntry{i, 1, "data" + std::to_string(i)})
                     .has_value());
    }

    CHECK_TRUE(store.install_snapshot(raft::SnapshotMetadata{3, 1}).has_value());
    CHECK_EQ(store.last_index().value_or(99), uint64_t{5});

    auto meta = store.snapshot_metadata();
    CHECK_TRUE(meta.has_value());
    CHECK_EQ(meta->last_included_index, uint64_t{3});
    CHECK_EQ(meta->last_included_term, uint64_t{1});

    // Below the boundary: gone. At the boundary: synthesized with the term.
    // Above the boundary: the real log entries survive.
    CHECK_FALSE(store.at(2).has_value());
    auto boundary = store.at(3);
    CHECK_TRUE(boundary.has_value());
    CHECK_EQ(boundary->term, uint64_t{1});
    CHECK_TRUE(boundary->data.empty());
    auto after = store.at(4);
    CHECK_TRUE(after.has_value());
    CHECK_STREQ(after->data, "data4");

    // Appending continues right after the highest index.
    CHECK_TRUE(store.append(raft::LogEntry{6, 1, "data6"}).has_value());
  }

  {
    raft::LevelDBLogStore store;
    CHECK_TRUE(store.open(directory.path()).has_value());
    CHECK_EQ(store.last_index().value_or(99), uint64_t{6});
    auto meta = store.snapshot_metadata();
    CHECK_TRUE(meta.has_value());
    CHECK_EQ(meta->last_included_index, uint64_t{3});
    CHECK_EQ(meta->last_included_term, uint64_t{1});
    CHECK_FALSE(store.at(2).has_value());
    CHECK_TRUE(store.at(3).has_value());
    CHECK_EQ(store.at(4)->data, "data4");
    CHECK_EQ(store.at(6)->data, "data6");
  }
}

TEST(RaftCore, LevelDBLogStoreRecoversAfterRestart) {
  TempDirectory directory;
  CHECK_TRUE(directory.valid());

  {
    ManualClock clock;
    TestNetwork network;
    raft::LevelDBLogStore log;
    RecordingStateMachine state_machine;
    TestTransport transport(NodeId{1}, network);
    raft::RaftNode node(
        raft::NodeConfig{
            NodeId{1}, std::vector<NodeId>{NodeId{1}}, 100, 10},
        log, transport, state_machine, clock);

    CHECK_TRUE(log.open(directory.path()).has_value());
    CHECK_TRUE(node.start().has_value());
    clock.advance(200);
    node.tick();

    CHECK_TRUE(node.is_leader());
    CHECK_EQ(node.term(), uint64_t{1});
    CHECK_EQ(node.last_log_index(), uint64_t{1});
    CHECK_EQ(node.commit_index(), uint64_t{1});
    auto proposal = node.propose("restart-me");
    CHECK_TRUE(proposal.has_value());
    CHECK_TRUE(proposal->committed);
    CHECK_EQ(node.last_log_index(), uint64_t{2});
    CHECK_EQ(node.commit_index(), uint64_t{2});
  }

  {
    ManualClock clock;
    TestNetwork network;
    raft::LevelDBLogStore log;
    RecordingStateMachine state_machine;
    TestTransport transport(NodeId{1}, network);
    raft::RaftNode node(
        raft::NodeConfig{
            NodeId{1}, std::vector<NodeId>{NodeId{1}}, 100, 10},
        log, transport, state_machine, clock);

    CHECK_TRUE(log.open(directory.path()).has_value());
    CHECK_TRUE(node.start().has_value());

    CHECK_EQ(node.term(), uint64_t{1});
    CHECK_EQ(node.role(), raft::Role::Follower);
    CHECK_EQ(node.last_log_index(), uint64_t{2});
    CHECK_EQ(node.commit_index(), uint64_t{0});

    clock.advance(200);
    node.tick();

    CHECK_TRUE(node.is_leader());
    CHECK_EQ(node.term(), uint64_t{2});
    CHECK_EQ(node.last_log_index(), uint64_t{3}); // new leadership no-op
    CHECK_EQ(node.commit_index(), uint64_t{3});
    CHECK_EQ(node.applied_index(), uint64_t{3});
    CHECK_TRUE(state_machine.contains("restart-me"));
  }
}

TEST(LevelDBRequestResultStore, PersistsLatestResultPerClient) {
  TempDirectory directory;
  CHECK_TRUE(directory.valid());
  const std::string result{"result\0binary", 14};

  {
    raft::LevelDBRequestResultStore store;
    CHECK_TRUE(store.open(directory.path()).has_value());
    auto missing = store.find(17, 9);
    CHECK_TRUE(missing.has_value());
    CHECK_FALSE(missing->has_value());

    CHECK_TRUE(store.save(17, 9, result).has_value());
  }

  {
    raft::LevelDBRequestResultStore store;
    CHECK_TRUE(store.open(directory.path()).has_value());
    auto found = store.find(17, 9);
    CHECK_TRUE(found.has_value());
    CHECK_TRUE(found->has_value());
    CHECK_EQ(**found, result);

    CHECK_TRUE(store.save(17, 10, "newer").has_value());
    auto stale = store.find(17, 9);
    CHECK_FALSE(stale.has_value());
    CHECK_EQ(stale.error().code, raft::ErrorCode::InvalidArgument);
  }
}

#endif

TEST(RaftCore, SingleNodeElectsAndCommitsImmediately) {
  TestCluster cluster(std::vector<NodeId>{NodeId{1}});
  cluster.start();

  for (int i = 0; i < 20; ++i) {
    cluster.step();
  }

  raft::RaftNode *leader = cluster.leader();
  CHECK_NOT_NULL(leader);
  CHECK_EQ(leader->term(), 1);
  CHECK_EQ(leader->last_log_index(), 1); // leadership no-op
  CHECK_EQ(leader->commit_index(), 1);
  CHECK_EQ(leader->applied_index(), 1);

  auto proposal = leader->propose("value");
  CHECK_TRUE(proposal.has_value());
  CHECK_EQ(proposal->index, 2);
  CHECK_EQ(proposal->term, 1);
  CHECK_TRUE(proposal->committed);
  CHECK_EQ(leader->commit_index(), 2);
  CHECK_EQ(leader->applied_index(), 2);
  CHECK_TRUE(cluster.test_node(NodeId{1}).state_machine.contains("value"));
}

TEST(RaftCore, ThreeNodesElectReplicateCommitAndApply) {
  TestCluster cluster(
      std::vector<NodeId>{NodeId{1}, NodeId{2}, NodeId{3}});
  cluster.start();

  for (int i = 0; i < 20; ++i) {
    cluster.step();
  }

  raft::RaftNode *leader = cluster.leader();
  CHECK_NOT_NULL(leader);
  CHECK_EQ(leader->term(), 1);
  CHECK_EQ(leader->last_log_index(), 1);
  CHECK_EQ(leader->commit_index(), 1);

  for (const NodeId id :
       std::vector<NodeId>{NodeId{1}, NodeId{2}, NodeId{3}}) {
    raft::RaftNode *node = cluster.node(id);
    CHECK_EQ(node->commit_index(), 1);
    CHECK_EQ(node->applied_index(), 1);
    CHECK_EQ(node->leader_hint(), NodeId{1});
  }

  auto proposal = leader->propose("alpha");
  CHECK_TRUE(proposal.has_value());
  CHECK_EQ(proposal->index, 2);
  CHECK_FALSE(proposal->committed);
  CHECK_FALSE(proposal->done());
  auto timeout = proposal->wait_for(std::chrono::milliseconds{1});
  CHECK_FALSE(timeout.has_value());
  CHECK_EQ(timeout.error().code, raft::ErrorCode::Timeout);

  for (int i = 0; i < 10; ++i) {
    cluster.step();
  }

  auto committed = proposal->wait();
  CHECK_TRUE(committed.has_value());
  CHECK_TRUE(committed->committed);
  CHECK_STREQ(committed->apply_result, "alpha");

  for (const NodeId id :
       std::vector<NodeId>{NodeId{1}, NodeId{2}, NodeId{3}}) {
    raft::RaftNode *node = cluster.node(id);
    CHECK_EQ(node->last_log_index(), 2);
    CHECK_EQ(node->commit_index(), 2);
    CHECK_EQ(node->applied_index(), 2);
    CHECK_TRUE(cluster.test_node(id).state_machine.contains("alpha"));
    CHECK_FALSE(node->last_error().has_value());
  }
}

TEST(RaftCore, ProposalWaitsThroughLeadershipLoss) {
  TestCluster cluster(
      std::vector<NodeId>{NodeId{1}, NodeId{2}, NodeId{3}});
  cluster.start();
  for (int i = 0; i < 20; ++i) {
    cluster.step();
  }

  raft::RaftNode *leader = cluster.leader();
  CHECK_NOT_NULL(leader);
  auto proposal = leader->propose("lost");
  CHECK_TRUE(proposal.has_value());
  CHECK_FALSE(proposal->done());

  leader->handle_message(
      NodeId{3},
      raft::AppendEntriesRequest{
          leader->term() + 1, NodeId{3}, 0, 0, {}, 0});

  auto result = proposal->wait_for(std::chrono::milliseconds{100});
  CHECK_FALSE(result.has_value());
  CHECK_EQ(result.error().code, raft::ErrorCode::NotLeader);
}

TEST(RaftCore, ProposalReportsStateMachineFailure) {
  class FailingStateMachine final : public raft::StateMachine {
  public:
    std::expected<std::string, raft::Error>
    apply(const raft::LogEntry &entry) override {
      if (entry.data.empty()) {
        return std::string{"noop"};
      }
      return std::unexpected(
          raft::Error{raft::ErrorCode::IOError, "injected apply failure"});
    }

    std::expected<std::string, raft::Error>
    snapshot(kv::KeyRange) override {
      return std::string{};
    }

    std::expected<void, raft::Error> restore(std::string_view) override {
      return {};
    }
  };

  ManualClock clock;
  TestNetwork network;
  raft::MemoryLogStore log;
  FailingStateMachine state_machine;
  TestTransport transport(NodeId{1}, network);
  raft::RaftNode node(
      raft::NodeConfig{
          NodeId{1}, std::vector<NodeId>{NodeId{1}}, 100, 10},
      log, transport, state_machine, clock);
  CHECK_TRUE(node.start().has_value());
  clock.advance(200);
  node.tick();
  CHECK_TRUE(node.is_leader());

  auto proposal = node.propose("will-fail");
  CHECK_TRUE(proposal.has_value());
  auto result = proposal->wait_for(std::chrono::milliseconds{100});
  CHECK_FALSE(result.has_value());
  CHECK_EQ(result.error().code, raft::ErrorCode::IOError);
  CHECK_STREQ(result.error().message, "injected apply failure");
}

TEST(RaftCore, ReadBarrierWaitsForQuorumConfirmation) {
  TestCluster cluster(
      std::vector<NodeId>{NodeId{1}, NodeId{2}, NodeId{3}});
  cluster.start();

  for (int i = 0; i < 15; ++i) {
    cluster.tick_one(NodeId{1});
  }

  raft::RaftNode *leader = cluster.node(NodeId{1});
  leader->handle_message(
      NodeId{2}, raft::RequestVoteResponse{leader->term(), true});
  leader->handle_message(
      NodeId{3}, raft::RequestVoteResponse{leader->term(), true});
  CHECK_TRUE(leader->is_leader());

  // A fresh leader has not committed anything yet: the read index is the
  // commit index at request time, and serving it still requires a heartbeat
  // round that starts after the request.
  auto read_index = leader->read_barrier();
  CHECK_TRUE(read_index.has_value());
  CHECK_EQ(read_index->index, uint64_t{0});
  CHECK_FALSE(read_index->done());

  auto timeout = read_index->wait_for(std::chrono::milliseconds{1});
  CHECK_FALSE(timeout.has_value());
  CHECK_EQ(timeout.error().code, raft::ErrorCode::Timeout);

  cluster.deliver_messages();
  auto ready = read_index->wait_for(std::chrono::milliseconds{100});
  CHECK_TRUE(ready.has_value());
  CHECK_EQ(ready->index, uint64_t{0});
}

TEST(RaftCore, ReadBarrierIsImmediateOnSingleNode) {
  TestCluster cluster(std::vector<NodeId>{NodeId{1}});
  cluster.start();
  cluster.step(200);
  CHECK_TRUE(cluster.node(NodeId{1})->is_leader());

  auto read_index = cluster.node(NodeId{1})->read_barrier();
  CHECK_TRUE(read_index.has_value());
  CHECK_TRUE(read_index->done());
  CHECK_EQ(read_index->index, cluster.node(NodeId{1})->commit_index());
}

TEST(RaftCore, ReadBarrierIgnoresStaleAcknowledgements) {
  TestCluster cluster(
      std::vector<NodeId>{NodeId{1}, NodeId{2}, NodeId{3}});
  cluster.start();
  for (int i = 0; i < 20; ++i) {
    cluster.step();
  }

  raft::RaftNode *leader = cluster.leader();
  CHECK_NOT_NULL(leader);
  const NodeId leader_id = leader->node_id();

  std::vector<NodeId> followers;
  for (const NodeId id : std::vector<NodeId>{NodeId{1}, NodeId{2}, NodeId{3}}) {
    if (id != leader_id) {
      followers.push_back(id);
    }
  }
  // Delay both followers' responses: their requests are delivered, their
  // acknowledgements are parked for a later replay.
  for (const NodeId id : followers) {
    cluster.hold(id, leader_id);
  }

  auto first = leader->read_barrier();
  CHECK_TRUE(first.has_value());
  cluster.deliver_messages();
  CHECK_EQ(cluster.held_count(), size_t{2});
  CHECK_FALSE(first->done());

  auto second = leader->read_barrier();
  CHECK_TRUE(second.has_value());
  cluster.deliver_messages();
  CHECK_EQ(cluster.held_count(), size_t{4});
  CHECK_FALSE(second->done());

  // Replaying one first-round acknowledgement satisfies the first barrier
  // only. Crediting it to the second barrier would be exactly the stale-ack
  // bug: it was produced before that read request existed.
  cluster.release_held(1);
  CHECK_TRUE(first->done());
  CHECK_FALSE(second->done());

  cluster.release_held(3);
  CHECK_TRUE(second->done());
}

TEST(RaftCore, IsolatedLeaderCannotServeReads) {
  TestCluster cluster(
      std::vector<NodeId>{NodeId{1}, NodeId{2}, NodeId{3}});
  cluster.start();
  for (int i = 0; i < 20; ++i) {
    cluster.step();
  }

  raft::RaftNode *leader = cluster.leader();
  CHECK_NOT_NULL(leader);
  const uint64_t committed_before = leader->commit_index();

  // The leader still thinks it leads, but a partition means the heartbeat
  // round can never be acknowledged by a majority. Reads must not be served
  // from this stale state.
  cluster.block(leader->node_id());
  auto read_index = leader->read_barrier();
  CHECK_TRUE(read_index.has_value());
  CHECK_EQ(read_index->index, committed_before);
  auto result = read_index->wait_for(std::chrono::milliseconds{10});
  CHECK_FALSE(result.has_value());
  CHECK_EQ(result.error().code, raft::ErrorCode::Timeout);
}

TEST(RaftCore, ReadBarrierFailsOnLeadershipLoss) {
  TestCluster cluster(
      std::vector<NodeId>{NodeId{1}, NodeId{2}, NodeId{3}});
  cluster.start();

  for (int i = 0; i < 15; ++i) {
    cluster.tick_one(NodeId{1});
  }
  raft::RaftNode *leader = cluster.node(NodeId{1});
  leader->handle_message(
      NodeId{2}, raft::RequestVoteResponse{leader->term(), true});
  leader->handle_message(
      NodeId{3}, raft::RequestVoteResponse{leader->term(), true});

  auto read_index = leader->read_barrier();
  CHECK_TRUE(read_index.has_value());
  CHECK_FALSE(read_index->done());

  leader->handle_message(
      NodeId{3},
      raft::AppendEntriesRequest{
          leader->term() + 1, NodeId{3}, 0, 0, {}, 0});

  auto result = read_index->wait_for(std::chrono::milliseconds{100});
  CHECK_FALSE(result.has_value());
  CHECK_EQ(result.error().code, raft::ErrorCode::NotLeader);
}

TEST(RaftCore, FollowerReadBarrierReturnsNotLeader) {
  TestCluster cluster(
      std::vector<NodeId>{NodeId{1}, NodeId{2}, NodeId{3}});
  cluster.start();
  for (int i = 0; i < 20; ++i) {
    cluster.step();
  }

  raft::RaftNode *follower = cluster.node(NodeId{2});
  CHECK_FALSE(follower->is_leader());
  auto result = follower->read_barrier();
  CHECK_FALSE(result.has_value());
  CHECK_EQ(result.error().code, raft::ErrorCode::NotLeader);
}

TEST(RaftCore, FollowerRejectsProposal) {
  TestCluster cluster(
      std::vector<NodeId>{NodeId{1}, NodeId{2}, NodeId{3}});
  cluster.start();
  for (int i = 0; i < 20; ++i) {
    cluster.step();
  }

  raft::RaftNode *follower = cluster.node(NodeId{2});
  CHECK_FALSE(follower->is_leader());
  auto result = follower->propose("not-leader");
  CHECK_FALSE(result.has_value());
  CHECK_EQ(result.error().code, raft::ErrorCode::NotLeader);
}

TEST(RaftCore, HigherTermStepsLeaderDown) {
  TestCluster cluster(
      std::vector<NodeId>{NodeId{1}, NodeId{2}, NodeId{3}});
  cluster.start();
  for (int i = 0; i < 20; ++i) {
    cluster.step();
  }

  raft::RaftNode *old_leader = cluster.leader();
  CHECK_NOT_NULL(old_leader);
  old_leader->handle_message(
      NodeId{3},
      raft::AppendEntriesRequest{
          2, NodeId{3}, 0, 0, {}, 0});

  CHECK_FALSE(old_leader->is_leader());
  CHECK_EQ(old_leader->role(), raft::Role::Follower);
  CHECK_EQ(old_leader->term(), 2);
  CHECK_EQ(old_leader->leader_hint(), NodeId{3});

  auto result = old_leader->propose("after-stepdown");
  CHECK_FALSE(result.has_value());
  CHECK_EQ(result.error().code, raft::ErrorCode::NotLeader);
}

TEST(RaftCore, FollowerTruncatesConflictingSuffix) {
  ManualClock clock;
  TestNetwork network;
  raft::MemoryLogStore log;
  RecordingStateMachine state_machine;

  CHECK_TRUE(log.append(raft::LogEntry{1, 1, "stale"}).has_value());
  CHECK_TRUE(
      log.save_hard_state(raft::HardState{1, NodeId{1}}).has_value());

  TestTransport transport(NodeId{1}, network);
  raft::RaftNode node(
      raft::NodeConfig{
          NodeId{1},
          std::vector<NodeId>{NodeId{1}, NodeId{2}, NodeId{3}},
          100,
          10},
      log, transport, state_machine, clock);
  CHECK_TRUE(node.start().has_value());

  node.handle_message(
      NodeId{2},
      raft::AppendEntriesRequest{
          2,
          NodeId{2},
          0,
          0,
          std::vector<raft::LogEntryMessage>{
              raft::LogEntryMessage{raft::LogEntry{1, 2, "good"}}},
          1});
  network.deliver_all();

  CHECK_EQ(node.term(), 2);
  CHECK_EQ(node.role(), raft::Role::Follower);
  CHECK_EQ(node.last_log_index(), 1);
  CHECK_EQ(node.commit_index(), 1);
  CHECK_EQ(node.applied_index(), 1);
  auto entry = log.at(1);
  CHECK_TRUE(entry.has_value());
  CHECK_EQ(entry->term, 2);
  CHECK_STREQ(entry->data, "good");
  CHECK_TRUE(state_machine.contains("good"));
}

TEST(MemoryLogStore, CompactionHidesPrefixAndKeepsAppending) {
  raft::MemoryLogStore log;
  for (uint64_t i = 1; i <= 4; ++i) {
    CHECK_TRUE(log.append(raft::LogEntry{i, 1, "d" + std::to_string(i)})
                   .has_value());
  }
  CHECK_TRUE(log.install_snapshot(raft::SnapshotMetadata{3, 1}).has_value());

  CHECK_EQ(log.last_index().value_or(99), uint64_t{4});
  CHECK_FALSE(log.at(2).has_value()); // inside the compacted prefix
  auto boundary = log.at(3);
  CHECK_TRUE(boundary.has_value());
  CHECK_EQ(boundary->term, uint64_t{1});
  CHECK_TRUE(boundary->data.empty());
  CHECK_EQ(log.at(4)->data, "d4");

  // Appending and truncation continue to work after compaction.
  CHECK_TRUE(log.append(raft::LogEntry{5, 1, "d5"}).has_value());
  CHECK_TRUE(log.truncate_suffix(5).has_value());
  CHECK_EQ(log.last_index().value_or(99), uint64_t{4});
  CHECK_FALSE(log.at(5).has_value());
}

TEST(RaftCore, SingleNodeCompactsLogAndContinues) {
  ManualClock clock;
  TestNetwork network;
  raft::MemoryLogStore log;
  RecordingStateMachine state_machine;
  TestTransport transport(NodeId{1}, network);
  raft::RaftNode node(
      raft::NodeConfig{NodeId{1}, std::vector<NodeId>{NodeId{1}}, 100, 10,
                       kv::KeyRange::from(kv::Key{}), 3},
      log, transport, state_machine, clock);
  CHECK_TRUE(node.start().has_value());

  clock.advance(200);
  node.tick();
  CHECK_TRUE(node.is_leader());

  for (uint64_t i = 1; i <= 4; ++i) {
    auto proposal = node.propose("entry-" + std::to_string(i));
    CHECK_TRUE(proposal.has_value());
    CHECK_TRUE(proposal->done()); // single member commits immediately
  }
  // Leadership added a no-op at index 1; the four entries land at 2..5.
  CHECK_EQ(node.last_log_index(), 5);
  CHECK_EQ(node.applied_index(), 5);

  // Compaction triggers on tick once the threshold is reached.
  node.tick();
  CHECK_GE(node.last_included_index(), 3);
  CHECK_EQ(node.last_included_term(), 1);
  CHECK_FALSE(log.at(1).has_value()); // compacted prefix is gone
  const uint64_t included = node.last_included_index();

  // A restart over the same log store resumes from the snapshot boundary.
  RecordingStateMachine restarted_machine;
  TestNetwork network2;
  TestTransport transport2(NodeId{1}, network2);
  raft::RaftNode restarted(
      raft::NodeConfig{NodeId{1}, std::vector<NodeId>{NodeId{1}}, 100, 10,
                       kv::KeyRange::from(kv::Key{}), 3},
      log, transport2, restarted_machine, clock);
  CHECK_TRUE(restarted.start().has_value());
  CHECK_EQ(restarted.last_included_index(), included);
  CHECK_EQ(restarted.applied_index(), included);
  CHECK_EQ(restarted.commit_index(), included);

  // The new leadership appends a no-op right after the compacted prefix, then
  // a real proposal continues past it.
  clock.advance(200);
  restarted.tick();
  CHECK_TRUE(restarted.is_leader());
  CHECK_EQ(restarted.last_log_index(), included + 1);

  auto proposal = restarted.propose("after");
  CHECK_TRUE(proposal.has_value());
  CHECK_TRUE(proposal->done());
  CHECK_EQ(restarted.last_log_index(), included + 2);
  CHECK_EQ(restarted.applied_index(), included + 2);
}

TEST(RaftCore, NewFollowerCatchesUpViaSnapshot) {
  using raft::NodeId;
  ManualClock clock;
  TestNetwork network;
  const std::vector<NodeId> peers{NodeId{1}, NodeId{2}, NodeId{3}};
  const uint64_t threshold = 3;

  auto make_node = [&](NodeId id) -> std::unique_ptr<TestNode> {
    auto test_node = std::make_unique<TestNode>();
    test_node->transport = std::make_unique<TestTransport>(id, network);
    test_node->node = std::make_unique<raft::RaftNode>(
        raft::NodeConfig{id, peers, 100, 10, kv::KeyRange::from(kv::Key{}),
                         threshold},
        test_node->log, *test_node->transport, test_node->state_machine,
        clock);
    network.bind(id, test_node->node.get());
    return test_node;
  };

  auto n1 = make_node(NodeId{1});
  auto n2 = make_node(NodeId{2});

  std::vector<raft::RaftNode *> live{n1->node.get(), n2->node.get()};
  auto step = [&](uint64_t ms = 10) {
    clock.advance(ms);
    for (raft::RaftNode *n : live) {
      n->tick();
    }
    network.deliver_all();
  };

  CHECK_TRUE(n1->node->start().has_value());
  CHECK_TRUE(n2->node->start().has_value());
  for (int i = 0; i < 20; ++i) {
    step();
  }

  // With only nodes 1 and 2 up, node 1 campaigns first and wins.
  raft::RaftNode *leader =
      n1->node->is_leader() ? n1->node.get() : n2->node.get();
  CHECK_TRUE(leader->is_leader());
  CHECK_EQ(leader, n1->node.get());

  auto propose_and_wait = [&](const std::string &data) {
    auto proposal = leader->propose(data);
    CHECK_TRUE(proposal.has_value());
    for (int i = 0; i < 200 && !proposal->done(); ++i) {
      step();
    }
    auto committed = proposal->wait();
    CHECK_TRUE(committed.has_value());
    return committed;
  };

  for (uint64_t i = 1; i <= 4; ++i) {
    propose_and_wait("pre-" + std::to_string(i));
  }
  CHECK_GE(leader->applied_index(), 4);

  // Let the leader compact its log at the applied index.
  for (int i = 0; i < 5; ++i) {
    step();
  }
  const uint64_t included = leader->last_included_index();
  CHECK_GE(included, 3);
  CHECK_FALSE(n1->log.at(1).has_value()); // compacted prefix is gone

  // A brand-new follower joins after the log was compacted: it must catch up
  // by installing the leader's snapshot.
  auto n3 = make_node(NodeId{3});
  CHECK_TRUE(n3->node->start().has_value());
  live.push_back(n3->node.get());
  for (int i = 0; i < 40; ++i) {
    step();
  }

  CHECK_EQ(n3->node->last_included_index(), included);
  CHECK_EQ(n3->node->last_log_index(), leader->last_log_index());
  CHECK_EQ(n3->node->applied_index(), leader->applied_index());
  CHECK_TRUE(n3->state_machine.restored);

  // Post-snapshot proposals still replicate to the new follower.
  auto p1 = propose_and_wait("post-1");
  auto p2 = propose_and_wait("post-2");
  CHECK_STREQ(p1->apply_result, "post-1");
  CHECK_STREQ(p2->apply_result, "post-2");
  CHECK_EQ(n3->node->last_log_index(), leader->last_log_index());
  CHECK_EQ(n3->node->applied_index(), leader->applied_index());
  CHECK_TRUE(n3->state_machine.contains("post-1"));
  CHECK_TRUE(n3->state_machine.contains("post-2"));
}

TEST(ProposalPayload, RoundTripsAllWriteBatchOperations) {
  raft::ProposalPayload payload;
  payload.client_id = 42;
  payload.request_id = 99;
  payload.batch.put(std::string{"key\0one", 7},
                   std::string{"value\0two", 10});
  payload.batch.remove("key-two");
  payload.batch.remove_range("range-a", "range-b");

  const std::string encoded = raft::encode_proposal_payload(payload);
  auto decoded = raft::decode_proposal_payload(encoded);
  CHECK_TRUE(decoded.has_value());
  CHECK_EQ(decoded->client_id, uint64_t{42});
  CHECK_EQ(decoded->request_id, uint64_t{99});
  CHECK_EQ(decoded->batch.size(), size_t{3});

  const auto &ops = decoded->batch.ops();
  CHECK_EQ(ops[0].type, kv::WriteBatch::OpType::kPut);
  const std::string expected_key{"key\0one", 7};
  const std::string expected_value{"value\0two", 10};
  CHECK_EQ(ops[0].data.key, expected_key);
  CHECK_TRUE(ops[0].data.value.has_value());
  if (ops[0].data.value.has_value()) {
    CHECK_EQ(ops[0].data.value.value(), expected_value);
  }
  CHECK_EQ(ops[1].type, kv::WriteBatch::OpType::kRemove);
  CHECK_EQ(ops[1].data.key, std::string{"key-two"});
  CHECK_EQ(ops[2].type, kv::WriteBatch::OpType::kRemoveRange);
  CHECK_EQ(ops[2].data.key, std::string{"range-a"});
  CHECK_EQ(ops[2].range_end, std::string{"range-b"});
}

TEST(ProposalPayload, RejectsMalformedData) {
  CHECK_FALSE(raft::decode_proposal_payload("").has_value());
  CHECK_FALSE(raft::decode_proposal_payload("not-a-payload").has_value());

  raft::ProposalPayload payload;
  payload.client_id = 1;
  payload.request_id = 2;
  payload.batch.put("key", "value");
  std::string encoded = raft::encode_proposal_payload(payload);
  CHECK_TRUE(encoded.size() > 5);
  encoded[7] = 0; // version low byte
  auto decoded = raft::decode_proposal_payload(encoded);
  CHECK_FALSE(decoded.has_value());
  CHECK_EQ(decoded.error().code, raft::ErrorCode::InvalidArgument);

  encoded = raft::encode_proposal_payload(payload);
  encoded.pop_back();
  CHECK_FALSE(raft::decode_proposal_payload(encoded).has_value());
}

std::map<std::string, std::string> collect_range(
    const std::shared_ptr<kv::KVStore> &store, const kv::KeyRange &range) {
  std::map<std::string, std::string> result;
  auto iterator = store->new_iterator(range);
  if (iterator == nullptr) {
    return result;
  }
  for (iterator->seek_to_first(); iterator->valid(); iterator->next()) {
    result.emplace(iterator->key(), iterator->value());
  }
  return result;
}

std::shared_ptr<kv::KVStore> open_mock_store() {
  kv::DatabaseOptions options;
  options.set_path("raft-kv-state-machine-test");
  auto store = kv::create_store(kv::EngineType::MOCK);
  if (store == nullptr || store->open(options) != kv::Status::OK) {
    return nullptr;
  }
  return store;
}

TEST(KVStateMachine, AppliesPayloadAndDeduplicatesRequestId) {
  auto local = open_mock_store();
  CHECK_TRUE(local != nullptr);
  raft::MemoryRequestResultStore request_results;
  raft::KVStateMachine state_machine{local, request_results};

  raft::ProposalPayload payload;
  payload.client_id = 17;
  payload.request_id = 31;
  const std::string key{"key\0one", 7};
  const std::string value{"value\0one", 10};
  payload.batch.put(key, value);
  payload.batch.put("key-two", "value-two");
  payload.batch.remove("removed-key");
  payload.batch.remove_range("old-a", "old-b");

  const raft::LogEntry entry{1, 1,
                              raft::encode_proposal_payload(payload)};
  auto first = state_machine.apply(entry);
  CHECK_TRUE(first.has_value());
  CHECK_STREQ(*first, "applied");

  const auto values = collect_range(
      local, kv::KeyRange::all());
  CHECK_EQ(values.size(), size_t{2});
  if (values.size() == 2) {
    CHECK_EQ(values.at(key), value);
    CHECK_EQ(values.at("key-two"), std::string{"value-two"});
  }

  auto duplicate = state_machine.apply(entry);
  CHECK_TRUE(duplicate.has_value());
  CHECK_STREQ(*duplicate, "applied");
}

TEST(KVStateMachine, RejectsMalformedEntryWithoutMutatingKV) {
  auto local = open_mock_store();
  CHECK_TRUE(local != nullptr);
  raft::MemoryRequestResultStore request_results;
  raft::KVStateMachine state_machine{local, request_results};

  auto result = state_machine.apply(raft::LogEntry{1, 1, "not-a-payload"});
  CHECK_FALSE(result.has_value());
  CHECK_EQ(result.error().code, raft::ErrorCode::InvalidArgument);
  CHECK_TRUE(collect_range(local, kv::KeyRange::all()).empty());
}

TEST(KVStateMachine, SnapshotsAndRestoresFiniteRange) {
  auto source = open_mock_store();
  CHECK_TRUE(source != nullptr);
  raft::MemoryRequestResultStore ignored_results;
  raft::KVStateMachine source_state_machine{source, ignored_results};

  kv::WriteBatch initial;
  initial.put("range-a", "one");
  initial.put("range-b", std::string{"two\0three", 9});
  initial.put("outside", "do-not-copy");
  CHECK_EQ(source->write_batch(initial), kv::Status::OK);

  const kv::KeyRange range =
      kv::KeyRange::range("range-a", "range-z");
  auto snapshot = source_state_machine.snapshot(range);
  CHECK_TRUE(snapshot.has_value());

  auto target = open_mock_store();
  CHECK_TRUE(target != nullptr);
  raft::MemoryRequestResultStore target_results;
  raft::KVStateMachine target_state_machine{target, target_results};

  kv::WriteBatch stale;
  stale.put("range-a", "stale");
  stale.put("range-y", "stale");
  stale.put("outside", "stale");
  CHECK_EQ(target->write_batch(stale), kv::Status::OK);

  auto restored =
      target_state_machine.restore(range, *snapshot);
  CHECK_TRUE(restored.has_value());

  const auto values = collect_range(target, kv::KeyRange::all());
  CHECK_EQ(values.size(), size_t{3});
  if (values.size() == 3) {
    CHECK_EQ(values.at("range-a"), std::string{"one"});
    const std::string expected_range_value{"two\0three", 9};
    CHECK_EQ(values.at("range-b"), expected_range_value);
    CHECK_EQ(values.at("outside"), std::string{"stale"});
  }
}

TEST(KVStateMachine, SnapshotsAndRestoresWholeRangeWithoutUpperBound) {
  auto source = open_mock_store();
  CHECK_TRUE(source != nullptr);
  raft::MemoryRequestResultStore ignored_results;
  raft::KVStateMachine source_state_machine{source, ignored_results};

  kv::WriteBatch initial;
  initial.put("a", "one");
  initial.put("b", std::string{"two\0three", 9});
  initial.put("@system/meta", "meta");
  CHECK_EQ(source->write_batch(initial), kv::Status::OK);

  // P1 single group covers the whole key space: start = "" with no upper
  // bound. The snapshot must scan everything and restore over any stale keys.
  const kv::KeyRange whole = kv::KeyRange::from(kv::Key{});
  auto snapshot = source_state_machine.snapshot(whole);
  CHECK_TRUE(snapshot.has_value());

  auto target = open_mock_store();
  CHECK_TRUE(target != nullptr);
  raft::MemoryRequestResultStore target_results;
  raft::KVStateMachine target_state_machine{target, target_results};

  kv::WriteBatch stale;
  stale.put("a", "stale");
  stale.put("z", "stale");
  CHECK_EQ(target->write_batch(stale), kv::Status::OK);

  auto restored = target_state_machine.restore(whole, *snapshot);
  CHECK_TRUE(restored.has_value());

  const auto values = collect_range(target, kv::KeyRange::all());
  CHECK_EQ(values.size(), size_t{3});
  if (values.size() == 3) {
    CHECK_EQ(values.at("a"), std::string{"one"});
    const std::string expected_b{"two\0three", 9};
    CHECK_EQ(values.at("b"), expected_b);
    CHECK_EQ(values.at("@system/meta"), std::string{"meta"});
  }
}

TEST(KVStateMachine, SnapshotRestoreRejectsInvalidRange) {
  auto local = open_mock_store();
  CHECK_TRUE(local != nullptr);
  raft::MemoryRequestResultStore request_results;
  raft::KVStateMachine state_machine{local, request_results};

  const std::string snapshot_data{"SQSN"};
  auto result = state_machine.restore(kv::KeyRange::all(), snapshot_data);
  CHECK_FALSE(result.has_value());
  CHECK_EQ(result.error().code, raft::ErrorCode::InvalidArgument);
}

} // namespace
