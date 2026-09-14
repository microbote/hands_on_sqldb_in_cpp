#pragma once

#include <cstdint>
#include <expected>
#include <map>
#include <optional>
#include <set>
#include <string>

#include "raft/clock.h"
#include "raft/log_store.h"
#include "raft/state_machine.h"
#include "raft/transport.h"

namespace raft {

// A single node in a single Raft group.
//
// P0 intentionally has no threads and no sockets. All methods must be called
// from one scheduler; production code will serialize ticks and messages on a
// dedicated Raft service.
class RaftNode {
public:
  RaftNode(NodeConfig config, LogStore &log_store, Transport &transport,
           StateMachine &state_machine, Clock &clock);

  RaftNode(const RaftNode &) = delete;
  RaftNode &operator=(const RaftNode &) = delete;

  // Loads HardState, installs the transport callback, and enters follower
  // state. It does not wait for an election.
  std::expected<void, Error> start();

  // Drives election and heartbeat timers. The caller controls logical time.
  void tick();

  // Appends a proposal as leader. In a single-member group it commits and
  // applies immediately; in a multi-member group commit is driven by
  // AppendEntries responses.
  std::expected<Proposal, Error> propose(std::string data);

  // Creates a leader read barrier (ReadIndex). The returned index is the
  // leader's commit index at request time. It completes only after a majority
  // has acknowledged a heartbeat sent at or after this request (which proves
  // this leader still holds leadership for the current term) and the local
  // state machine has applied up to that index.
  std::expected<ReadIndex, Error> read_barrier();

  // Used by tests and by a production transport callback.
  void handle_message(NodeId from, const Message &message);

  bool is_leader() const { return role_ == Role::Leader; }
  Role role() const { return role_; }
  NodeId node_id() const { return config_.node_id; }
  uint64_t term() const { return hard_state_.term; }
  uint64_t election_timeout_ms() const { return config_.election_timeout_ms; }
  uint64_t commit_index() const { return commit_index_; }
  uint64_t applied_index() const { return last_applied_; }
  std::optional<NodeId> leader_hint() const { return leader_id_; }
  uint64_t last_log_index() const { return last_log_index_; }

  const std::optional<Error> &last_error() const { return last_error_; }

private:
  std::expected<void, Error> start_election();
  std::expected<void, Error> become_leader();
  std::expected<void, Error> become_follower(uint64_t term,
                                              std::optional<NodeId> leader_id);

  std::expected<void, Error>
  handle_request_vote(NodeId from, const RequestVoteRequest &request);
  void handle_request_vote_response(NodeId from,
                                    const RequestVoteResponse &response);
  std::expected<void, Error>
  handle_append_entries(NodeId from, const AppendEntriesRequest &request);
  void handle_append_entries_response(NodeId from,
                                       const AppendEntriesResponse &response);

  std::expected<void, Error> send_append_entries(NodeId to);
  void send_heartbeats();
  std::expected<void, Error> advance_commit();
  std::expected<void, Error> apply_committed();
  void complete_applied_proposal(uint64_t index,
                                 const std::string &apply_result);
  void fail_pending_proposals(std::optional<uint64_t> from_index,
                              Error error);
  void evaluate_read_barriers();
  void fail_read_barriers(Error error);
  bool quorum_acknowledged(uint64_t round) const;
  std::expected<uint64_t, Error> last_log_term() const;
  void reset_election_deadline();
  void record_error(Error error);

  uint64_t quorum_match_index() const;
  bool has_quorum(size_t count) const;
  void initialize_leader_progress();

  NodeConfig config_;
  LogStore &log_store_;
  Transport &transport_;
  StateMachine &state_machine_;
  Clock &clock_;

  HardState hard_state_{};
  Role role_ = Role::Follower;
  std::optional<NodeId> leader_id_;
  uint64_t last_log_index_ = kInvalidIndex;
  uint64_t commit_index_ = kInvalidIndex;
  uint64_t last_applied_ = kInvalidIndex;

  std::map<NodeId, uint64_t> next_index_;
  std::map<NodeId, uint64_t> match_index_;
  std::set<NodeId> votes_received_;
  std::map<uint64_t, std::shared_ptr<ProposalCompletion>> pending_proposals_;

  // ReadIndex bookkeeping: every heartbeat batch gets a round number, requests
  // carry it and responses echo it back. A read barrier waits for a majority
  // to acknowledge a round that started after the read request, so late
  // replies to older rounds can never be mistaken for fresh proof of
  // leadership.
  struct PendingReadBarrier {
    uint64_t round = 0;
    uint64_t target_index = kInvalidIndex;
    std::shared_ptr<Completion> completion;
  };
  std::vector<PendingReadBarrier> pending_read_barriers_;
  uint64_t heartbeat_round_ = 0;
  std::map<NodeId, uint64_t> peer_acked_round_;

  uint64_t election_deadline_ = 0;
  uint64_t next_heartbeat_ = 0;
  std::optional<Error> last_error_;
};

} // namespace raft
