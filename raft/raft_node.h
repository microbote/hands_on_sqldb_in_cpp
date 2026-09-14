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

  // Used by tests and by a production transport callback.
  void handle_message(NodeId from, const Message &message);

  bool is_leader() const { return role_ == Role::Leader; }
  Role role() const { return role_; }
  uint64_t term() const { return hard_state_.term; }
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

  uint64_t election_deadline_ = 0;
  uint64_t next_heartbeat_ = 0;
  std::optional<Error> last_error_;
};

} // namespace raft
