#pragma once

#include <cstdint>
#include <expected>
#include <optional>
#include <string>

#include "raft/raft_node.h"

namespace raft {

// How the SQL adapter submits work to Raft.
//
// Two implementations, and the difference matters:
//
//   - RaftRuntime (production): the call is queued to the raft service thread,
//     so it is safe from any session/read/write thread — and it is the *only*
//     thread that touches RaftNode;
//   - RaftNodeExecutor (tests and single-threaded drivers): calls the node
//     directly, which is only legal when the caller already owns the node.
class RaftExecutor {
public:
  virtual ~RaftExecutor() = default;

  virtual std::expected<Proposal, Error> propose(std::string data) = 0;
  virtual std::expected<ReadIndex, Error> read_barrier() = 0;
  virtual NodeId node_id() const = 0;
  virtual uint64_t election_timeout_ms() const = 0;
  // Wait budgets the adapter uses when blocking on a proposal or a read
  // barrier. 0 (the default) falls back to election_timeout_ms, keeping the
  // pre-configuration behavior.
  virtual uint64_t proposal_timeout_ms() const {
    return election_timeout_ms();
  }
  virtual uint64_t read_timeout_ms() const { return election_timeout_ms(); }
  // Leader id this node knows about (nullopt when unknown). Used to build the
  // redirect hint; the executor does not know client-facing addresses.
  virtual std::optional<NodeId> leader_hint() = 0;
};

// Direct calls: the caller is responsible for serializing access (single
// threaded test/driver). Production must use RaftRuntime.
class RaftNodeExecutor final : public RaftExecutor {
public:
  explicit RaftNodeExecutor(RaftNode &node) : node_(node) {}

  std::expected<Proposal, Error> propose(std::string data) override {
    return node_.propose(std::move(data));
  }
  std::expected<ReadIndex, Error> read_barrier() override {
    return node_.read_barrier();
  }
  NodeId node_id() const override { return node_.node_id(); }
  uint64_t election_timeout_ms() const override {
    return node_.election_timeout_ms();
  }
  std::optional<NodeId> leader_hint() override { return node_.leader_hint(); }

private:
  RaftNode &node_;
};

} // namespace raft
