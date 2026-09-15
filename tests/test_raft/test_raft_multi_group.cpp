// P2 adapter slice: multi-group routing through GroupRouter, cross-group
// transaction rules and per-group write slots. Each group is a single-node
// raft group sharing one local state-machine store, so the tests drive the
// adapter deterministically without sockets or stepping.

#include <map>
#include <memory>
#include <string>
#include <vector>

#include "raft/group_router.h"
#include "raft/kv_state_machine.h"
#include "raft/memory_log_store.h"
#include "raft/raft_executor.h"
#include "raft/raft_kv_store.h"
#include "raft/raft_node.h"
#include "raft/request_result_store.h"
#include "raft_test_net.h"
#include "storage/mock_engine/mock_engine.h"
#include "test_framework.h"

namespace {

using raft::NodeId;
using raft_test::ManualClock;
using raft_test::TestNetwork;
using raft_test::TestTransport;

struct Group {
  raft::MemoryLogStore log;
  raft::MemoryRequestResultStore results;
  std::shared_ptr<kv::MockStore> local;
  std::unique_ptr<raft::KVStateMachine> state_machine;
  std::unique_ptr<TestTransport> transport;
  std::unique_ptr<raft::RaftNode> node;
  std::unique_ptr<raft::RaftNodeExecutor> executor;
};

// Builds one single-node raft group over the shared state-machine store.
std::unique_ptr<Group> make_group(NodeId node_id, ManualClock &clock,
                                  TestNetwork &network,
                                  const std::shared_ptr<kv::MockStore> &local) {
  auto group = std::make_unique<Group>();
  group->local = local;
  group->state_machine =
      std::make_unique<raft::KVStateMachine>(group->local, group->results);
  group->transport = std::make_unique<TestTransport>(node_id, network);
  group->node = std::make_unique<raft::RaftNode>(
      raft::NodeConfig{node_id, std::vector<NodeId>{node_id}, 100, 10,
                       kv::KeyRange::from(kv::Key{}), 0},
      group->log, *group->transport, *group->state_machine, clock);
  network.bind(node_id, group->transport.get());
  group->executor = std::make_unique<raft::RaftNodeExecutor>(*group->node);
  return group;
}

void elect(Group &group, ManualClock &clock) {
  CHECK_TRUE(group.node->start().has_value());
  clock.advance(200);
  group.node->tick();
  CHECK_TRUE(group.node->is_leader());
}

// Three groups: 0 = @system/*, 1 = ["a","m"), 2 = ["m","z").
class MultiGroupFixture {
public:
  MultiGroupFixture() {
    local_ = std::make_shared<kv::MockStore>();
    CHECK_EQ(local_->open(kv::DatabaseOptions{}), kv::Status::OK);

    g0_ = make_group(NodeId{1}, clock0_, network0_, local_);
    g1_ = make_group(NodeId{2}, clock1_, network1_, local_);
    g2_ = make_group(NodeId{3}, clock2_, network2_, local_);
    elect(*g0_, clock0_);
    elect(*g1_, clock1_);
    elect(*g2_, clock2_);

    router_.add_range("a", "m", 1);
    router_.add_range("m", "z", 2);
    store_ = std::make_shared<raft::RaftKVStore>(
        local_,
        std::map<uint64_t, raft::RaftExecutor *>{
            {0, g0_->executor.get()},
            {1, g1_->executor.get()},
            {2, g2_->executor.get()}},
        router_);
    CHECK_EQ(store_->open(kv::DatabaseOptions{}), kv::Status::OK);
  }

  std::shared_ptr<kv::KVEngine> connect() { return store_->connect(); }
  raft::RaftKVStore &store() { return *store_; }
  Group &group(uint64_t id) {
    return id == 0 ? *g0_ : (id == 1 ? *g1_ : *g2_);
  }

private:
  std::shared_ptr<kv::MockStore> local_;
  ManualClock clock0_;
  ManualClock clock1_;
  ManualClock clock2_;
  TestNetwork network0_;
  TestNetwork network1_;
  TestNetwork network2_;
  std::unique_ptr<Group> g0_;
  std::unique_ptr<Group> g1_;
  std::unique_ptr<Group> g2_;
  raft::GroupRouter router_;
  std::shared_ptr<raft::RaftKVStore> store_;
};

TEST(GroupRouter, RoutesSystemAndDataRanges) {
  raft::GroupRouter router;
  router.add_range("a", "m", 1);
  router.add_range("m", "z", 2);

  CHECK_EQ(router.group_for("@system/databases"), uint64_t{0});
  CHECK_EQ(router.group_for("@system/tables/db"), uint64_t{0});
  CHECK_EQ(router.group_for(""), uint64_t{0}); // uncovered -> default
  CHECK_EQ(router.group_for("a"), uint64_t{1});
  CHECK_EQ(router.group_for("l"), uint64_t{1});
  CHECK_EQ(router.group_for("m"), uint64_t{2});
  CHECK_EQ(router.group_for("y"), uint64_t{2});
  CHECK_EQ(router.group_for("z"), uint64_t{0}); // ["m","z") excludes "z"
  CHECK_EQ(router.group_count(), size_t{3});

  CHECK_EQ(router.range_group("a", "l").value_or(99), uint64_t{1});
  CHECK_EQ(router.range_group("m", "y").value_or(99), uint64_t{2});
  CHECK_FALSE(router.range_group("a", "n").has_value()); // crosses "m"
  CHECK_EQ(router.range_group("@system/a", "@system/z").value_or(99),
           uint64_t{0});
  // An unbounded range starting in @system/* runs into the data groups.
  CHECK_FALSE(router.range_group("@system/a", std::nullopt).has_value());

  kv::WriteBatch single;
  single.put("a", "1");
  single.put("b", "2");
  CHECK_EQ(router.batch_group(single).value_or(99), uint64_t{1});

  kv::WriteBatch cross;
  cross.put("a", "1");
  cross.put("m", "2");
  CHECK_FALSE(router.batch_group(cross).has_value());

  kv::WriteBatch range_cross;
  range_cross.remove_range("a", "n"); // ["a","n") spans groups 1 and 2
  CHECK_FALSE(router.batch_group(range_cross).has_value());
}

TEST(GroupTransport, SharedTransportDispatchesByGroup) {
  TestNetwork network;
  auto transport = std::make_unique<TestTransport>(NodeId{1}, network);
  network.bind(NodeId{1}, transport.get());

  int group0 = 0;
  int group1 = 0;
  transport->on_message(0, [&](NodeId, const raft::Message &) { ++group0; });
  transport->on_message(1, [&](NodeId, const raft::Message &) { ++group1; });

  network.enqueue(NodeId{2}, NodeId{1}, 0,
                  raft::RequestVoteRequest{1, NodeId{2}, 0, 0});
  network.enqueue(NodeId{2}, NodeId{1}, 1,
                  raft::AppendEntriesRequest{1, NodeId{2}, 0, 0, {}, 1, 2});
  network.deliver_all();
  CHECK_EQ(group0, 1);
  CHECK_EQ(group1, 1);

  // A group nobody registered is dropped, not delivered to another group.
  network.enqueue(NodeId{2}, NodeId{1}, 7,
                  raft::RequestVoteResponse{1, true});
  network.deliver_all();
  CHECK_EQ(group0, 1);
  CHECK_EQ(group1, 1);
}

TEST(MultiGroup, WritesRouteToTheirOwnGroupStateMachine) {
  MultiGroupFixture fixture;
  auto engine = fixture.connect();

  CHECK_EQ(engine->put("a", "1"), kv::Status::OK);   // group 1
  CHECK_EQ(engine->put("m", "2"), kv::Status::OK);   // group 2
  CHECK_EQ(engine->put("@system/databases", "meta"), kv::Status::OK); // group 0

  CHECK_EQ(fixture.group(0).node->last_log_index(), uint64_t{2});
  CHECK_EQ(fixture.group(1).node->last_log_index(), uint64_t{2});
  CHECK_EQ(fixture.group(2).node->last_log_index(), uint64_t{2});

  kv::ByteValue value;
  CHECK_EQ(engine->get("a", &value), kv::Status::OK);
  CHECK_STREQ(value, "1");
  CHECK_EQ(engine->get("m", &value), kv::Status::OK);
  CHECK_STREQ(value, "2");
  CHECK_EQ(engine->get("@system/databases", &value), kv::Status::OK);
  CHECK_STREQ(value, "meta");
}

TEST(MultiGroup, ReadsRouteToTheOwningGroupBarrier) {
  MultiGroupFixture fixture;
  auto engine = fixture.connect();

  const uint64_t before0 = fixture.group(0).node->read_barrier_count();
  const uint64_t before1 = fixture.group(1).node->read_barrier_count();
  const uint64_t before2 = fixture.group(2).node->read_barrier_count();

  kv::ByteValue value;
  CHECK_EQ(engine->get("a", &value), kv::Status::NotFound);
  CHECK_EQ(fixture.group(1).node->read_barrier_count(), before1 + 1);
  CHECK_EQ(fixture.group(0).node->read_barrier_count(), before0);
  CHECK_EQ(fixture.group(2).node->read_barrier_count(), before2);

  // BEGIN takes one barrier per group before the snapshot.
  CHECK_EQ(engine->begin_transaction(), kv::Status::OK);
  CHECK_EQ(fixture.group(0).node->read_barrier_count(), before0 + 1);
  CHECK_EQ(fixture.group(1).node->read_barrier_count(), before1 + 2);
  CHECK_EQ(fixture.group(2).node->read_barrier_count(), before2 + 1);
  CHECK_EQ(engine->rollback_transaction(), kv::Status::OK);
}

TEST(MultiGroup, PerGroupWriteSlotsAreIndependent) {
  MultiGroupFixture fixture;
  auto writer_a = fixture.connect();
  auto writer_b = fixture.connect();
  auto writer_c = fixture.connect();

  CHECK_EQ(writer_a->begin_transaction(), kv::Status::OK);
  CHECK_EQ(writer_a->put("a", "x"), kv::Status::OK); // group 1 slot taken
  CHECK_TRUE(fixture.store().write_slot_held());

  // Same group: busy. Different group: free.
  CHECK_EQ(writer_b->put("b", "y"), kv::Status::Busy);
  CHECK_EQ(writer_c->put("m", "y"), kv::Status::OK);

  CHECK_EQ(writer_a->commit_transaction(), kv::Status::OK);
  CHECK_EQ(writer_b->put("b", "y"), kv::Status::OK);
  CHECK_FALSE(fixture.store().write_slot_held());
}

TEST(MultiGroup, CrossGroupWriteTransactionRejected) {
  MultiGroupFixture fixture;
  auto engine = fixture.connect();

  CHECK_EQ(engine->begin_transaction(), kv::Status::OK);
  CHECK_EQ(engine->put("a", "1"), kv::Status::OK);
  CHECK_EQ(engine->put("m", "2"), kv::Status::CrossGroupTransaction);
  const auto info = engine->last_cross_group_info();
  CHECK_TRUE(info.has_value());
  if (info.has_value()) {
    CHECK_EQ(info->from_group, uint64_t{1});
    CHECK_EQ(info->to_group, uint64_t{2});
  }
  CHECK_EQ(engine->rollback_transaction(), kv::Status::OK);

  kv::ByteValue value;
  CHECK_EQ(engine->get("a", &value), kv::Status::NotFound);
}

TEST(MultiGroup, CrossGroupReadStrictRejectedLooseAllowed) {
  MultiGroupFixture fixture;

  // strict (default): a second group's read is rejected.
  auto strict_engine = fixture.connect();
  CHECK_EQ(strict_engine->begin_transaction(), kv::Status::OK);
  kv::ByteValue value;
  CHECK_EQ(strict_engine->get("a", &value), kv::Status::NotFound);
  CHECK_EQ(strict_engine->get("m", &value), kv::Status::CrossGroupTransaction);
  const auto info = strict_engine->last_cross_group_info();
  CHECK_TRUE(info.has_value());
  if (info.has_value()) {
    CHECK_EQ(info->from_group, uint64_t{1});
    CHECK_EQ(info->to_group, uint64_t{2});
  }
  CHECK_EQ(strict_engine->rollback_transaction(), kv::Status::OK);

  // loose: the read is allowed, but the transaction freezes as read-only.
  auto loose_engine =
      std::dynamic_pointer_cast<raft::RaftKVEngine>(fixture.connect());
  CHECK_TRUE(loose_engine != nullptr);
  if (loose_engine == nullptr) {
    return;
  }
  loose_engine->set_loose_cross_group_reads(true);
  CHECK_EQ(loose_engine->begin_transaction(), kv::Status::OK);
  CHECK_EQ(loose_engine->get("a", &value), kv::Status::NotFound);
  CHECK_EQ(loose_engine->get("m", &value), kv::Status::NotFound);
  CHECK_EQ(loose_engine->put("a", "w"), kv::Status::CrossGroupTransaction);
  CHECK_EQ(loose_engine->rollback_transaction(), kv::Status::OK);

  // A write to another group is rejected even with no prior write.
  auto write_engine = fixture.connect();
  CHECK_EQ(write_engine->begin_transaction(), kv::Status::OK);
  CHECK_EQ(write_engine->get("a", &value), kv::Status::NotFound); // binds 1
  CHECK_EQ(write_engine->put("m", "w"), kv::Status::CrossGroupTransaction);
  CHECK_EQ(write_engine->rollback_transaction(), kv::Status::OK);
}

TEST(MultiGroup, AutoCommitCrossesGroupsRejected) {
  MultiGroupFixture fixture;
  auto engine = fixture.connect();

  kv::WriteBatch cross;
  cross.put("a", "1");
  cross.put("m", "2");
  CHECK_EQ(engine->write_batch(cross), kv::Status::CrossGroupTransaction);
  const auto info = engine->last_cross_group_info();
  CHECK_TRUE(info.has_value());
  if (info.has_value()) {
    CHECK_EQ(info->from_group, uint64_t{1});
    CHECK_EQ(info->to_group, uint64_t{2});
  }

  kv::WriteBatch within;
  within.put("a", "1");
  within.put("b", "2");
  CHECK_EQ(engine->write_batch(within), kv::Status::OK);

  kv::WriteBatch range_cross;
  range_cross.remove_range("a", "n");
  CHECK_EQ(engine->write_batch(range_cross), kv::Status::CrossGroupTransaction);

  std::vector<std::optional<kv::ByteValue>> values;
  CHECK_EQ(engine->get_batch({"a", "m"}, kv::MissingKeyPolicy::kReturnEmpty,
                             &values),
           kv::Status::CrossGroupTransaction);
}

TEST(MultiGroup, ScanSpanningGroupsRejected) {
  MultiGroupFixture fixture;
  auto engine = fixture.connect();
  CHECK_EQ(engine->put("a", "1"), kv::Status::OK);

  auto single_group_scan = engine->new_iterator(kv::KeyRange::range("a", "l"));
  CHECK_TRUE(single_group_scan != nullptr);
  if (single_group_scan == nullptr) {
    return;
  }
  CHECK_TRUE(single_group_scan->valid());
  CHECK_EQ(single_group_scan->key(), std::string("a"));

  auto cross_group_scan = engine->new_iterator(kv::KeyRange::range("a", "z"));
  CHECK_TRUE(cross_group_scan == nullptr);
  CHECK_EQ(engine->last_error(), kv::Status::CrossGroupTransaction);
}

} // namespace
