#include "raft/raft_node.h"

#include <algorithm>
#include <functional>
#include <type_traits>
#include <utility>

namespace raft {
namespace {

// Leader snapshot transfer chunk size. Kept far below the 64 MiB frame cap so
// a single chunk always fits in one frame.
constexpr size_t kSnapshotChunkBytes = 1u << 20;
// Follower-side guard against a broken leader filling memory with chunks.
constexpr size_t kMaxPendingSnapshotBytes = 1ull << 30;

bool log_is_up_to_date(uint64_t last_log_index, uint64_t last_log_term,
                       uint64_t candidate_index, uint64_t candidate_term) {
  if (candidate_term != last_log_term) {
    return candidate_term > last_log_term;
  }
  return candidate_index >= last_log_index;
}

} // namespace

RaftNode::RaftNode(NodeConfig config, LogStore &log_store,
                   Transport &transport, StateMachine &state_machine,
                   Clock &clock)
    : config_(std::move(config)), log_store_(log_store), transport_(transport),
      state_machine_(state_machine), clock_(clock) {
  if (config_.election_timeout_ms == 0) {
    config_.election_timeout_ms = 1;
  }
  if (config_.heartbeat_interval_ms == 0) {
    config_.heartbeat_interval_ms = 1;
  }
}

std::expected<void, Error> RaftNode::start() {
  if (config_.node_id.value == 0 || config_.peers.empty()) {
    return std::unexpected(
        Error{ErrorCode::InvalidArgument, "node id and peers are required"});
  }
  if (std::find(config_.peers.begin(), config_.peers.end(),
                config_.node_id) == config_.peers.end()) {
    return std::unexpected(Error{
        ErrorCode::InvalidArgument,
        "the local node id must be included in peers"});
  }
  const std::set<NodeId> unique_peers(config_.peers.begin(),
                                       config_.peers.end());
  if (unique_peers.size() != config_.peers.size()) {
    return std::unexpected(
        Error{ErrorCode::InvalidArgument, "peers must be unique"});
  }
  if (config_.heartbeat_interval_ms >= config_.election_timeout_ms) {
    return std::unexpected(Error{
        ErrorCode::InvalidArgument,
        "heartbeat interval must be smaller than election timeout"});
  }

  auto hard_state = log_store_.load_hard_state();
  if (!hard_state.has_value()) {
    return std::unexpected(hard_state.error());
  }
  hard_state_ = *hard_state;

  auto last_index = log_store_.last_index();
  if (!last_index.has_value()) {
    return std::unexpected(last_index.error());
  }
  last_log_index_ = *last_index;

  auto meta = log_store_.snapshot_metadata();
  if (!meta.has_value()) {
    return std::unexpected(meta.error());
  }
  last_included_index_ = meta->last_included_index;
  last_included_term_ = meta->last_included_term;

  // After a restart the local state machine already contains every applied
  // entry. With a compacted log the earliest recoverable index is the
  // snapshot boundary, so resume apply/commit accounting from there.
  last_applied_ = last_included_index_;
  commit_index_ = last_included_index_;

  role_ = Role::Follower;
  leader_id_.reset();
  votes_received_.clear();
  heartbeat_round_ = 0;
  peer_acked_round_.clear();
  pending_read_barriers_.clear();
  pending_snapshot_.reset();
  reset_election_deadline();

  transport_.on_message(
      [this](NodeId from, const Message &message) {
        handle_message(from, message);
      });
  return {};
}

void RaftNode::tick() {
  const uint64_t now = clock_.now_ms();
  if (role_ == Role::Leader) {
    if (now >= next_heartbeat_) {
      send_heartbeats();
      next_heartbeat_ = now + config_.heartbeat_interval_ms;
    }
  } else if (now >= election_deadline_) {
    if (auto result = start_election(); !result.has_value()) {
      record_error(result.error());
    }
  }

  if (auto result = apply_committed(); !result.has_value()) {
    record_error(result.error());
  }

  if (auto result = maybe_compact_log(); !result.has_value()) {
    record_error(result.error());
  }
}

std::expected<Proposal, Error> RaftNode::propose(std::string data) {
  if (role_ != Role::Leader) {
    return std::unexpected(
        Error{ErrorCode::NotLeader, "proposal requires the leader role"});
  }

  const uint64_t index = last_log_index_ + 1;
  LogEntry entry{index, hard_state_.term, data};
  if (auto result = log_store_.append(entry); !result.has_value()) {
    return std::unexpected(result.error());
  }

  last_log_index_ = index;
  match_index_[config_.node_id] = index;
  next_index_[config_.node_id] = index + 1;

  Proposal proposal{index, hard_state_.term, entry.data, false, {},
                    std::make_shared<ProposalCompletion>()};
  pending_proposals_[index] = proposal.completion;

  if (auto result = advance_commit(); !result.has_value()) {
    record_error(result.error());
  }
  send_heartbeats();

  proposal.committed = index <= commit_index_;
  return proposal;
}

std::expected<ReadIndex, Error> RaftNode::read_barrier() {
  ++read_barrier_count_;
  if (role_ != Role::Leader) {
    return std::unexpected(
        Error{ErrorCode::NotLeader, "read barrier requires the leader role"});
  }

  // ReadIndex: the read must see every entry that was committed before the
  // request arrived, so remember the commit index now. Proving leadership
  // needs a heartbeat round started *after* this point: acks that were already
  // in flight may predate a partition, and therefore cannot be used.
  const uint64_t target = commit_index_;
  send_heartbeats();
  const uint64_t round = heartbeat_round_;

  ReadIndex read_index{target, hard_state_.term,
                       std::make_shared<Completion>()};
  if (quorum_acknowledged(round) && last_applied_ >= target) {
    read_index.completion->finish(std::nullopt, {});
    return read_index;
  }

  pending_read_barriers_.push_back(
      PendingReadBarrier{round, target, read_index.completion});
  return read_index;
}

void RaftNode::handle_message(NodeId from, const Message &message) {
  std::visit(
      [&](const auto &concrete) {
        using T = std::decay_t<decltype(concrete)>;
        if constexpr (std::is_same_v<T, RequestVoteRequest>) {
          if (auto result = handle_request_vote(from, concrete);
              !result.has_value()) {
            record_error(result.error());
          }
        } else if constexpr (std::is_same_v<T, RequestVoteResponse>) {
          handle_request_vote_response(from, concrete);
        } else if constexpr (std::is_same_v<T, AppendEntriesRequest>) {
          if (auto result = handle_append_entries(from, concrete);
              !result.has_value()) {
            record_error(result.error());
          }
        } else if constexpr (std::is_same_v<T, AppendEntriesResponse>) {
          handle_append_entries_response(from, concrete);
        } else if constexpr (std::is_same_v<T, InstallSnapshotRequest>) {
          if (auto result = handle_install_snapshot(from, concrete);
              !result.has_value()) {
            record_error(result.error());
          }
        } else if constexpr (std::is_same_v<T, InstallSnapshotResponse>) {
          handle_install_snapshot_response(from, concrete);
        }
      },
      message);
}

std::expected<void, Error> RaftNode::start_election() {
  const HardState old_hard_state = hard_state_;
  const Role old_role = role_;

  role_ = Role::Candidate;
  ++hard_state_.term;
  hard_state_.voted_for = config_.node_id;
  leader_id_.reset();

  if (auto result = log_store_.save_hard_state(hard_state_);
      !result.has_value()) {
    hard_state_ = old_hard_state;
    role_ = old_role;
    return std::unexpected(result.error());
  }

  votes_received_.clear();
  votes_received_.insert(config_.node_id);
  reset_election_deadline();

  if (has_quorum(1)) {
    return become_leader();
  }

  auto last_term = last_log_term();
  if (!last_term.has_value()) {
    return std::unexpected(last_term.error());
  }

  for (const NodeId peer : config_.peers) {
    if (peer == config_.node_id) {
      continue;
    }
    transport_.send(peer,
                   RequestVoteRequest{hard_state_.term, config_.node_id,
                                      last_log_index_, *last_term});
  }
  return {};
}

std::expected<void, Error> RaftNode::become_leader() {
  if (role_ != Role::Candidate) {
    return {};
  }

  role_ = Role::Leader;
  leader_id_ = config_.node_id;
  initialize_leader_progress();

  // A no-op entry from the current term allows this leadership to commit
  // entries inherited from a previous term once a quorum stores it.
  const uint64_t index = last_log_index_ + 1;
  LogEntry no_op{index, hard_state_.term, {}};
  if (auto result = log_store_.append(no_op); !result.has_value()) {
    if (auto fallback = become_follower(hard_state_.term, std::nullopt);
        !fallback.has_value()) {
      return std::unexpected(fallback.error());
    }
    return std::unexpected(result.error());
  }

  last_log_index_ = index;
  match_index_[config_.node_id] = index;
  next_index_[config_.node_id] = index + 1;

  send_heartbeats();
  if (auto result = advance_commit(); !result.has_value()) {
    return std::unexpected(result.error());
  }
  return {};
}

std::expected<void, Error>
RaftNode::become_follower(uint64_t term, std::optional<NodeId> leader_id) {
  const Role old_role = role_;
  if (term > hard_state_.term) {
    const HardState old_hard_state = hard_state_;
    hard_state_.term = term;
    hard_state_.voted_for.reset();
    if (auto result = log_store_.save_hard_state(hard_state_);
        !result.has_value()) {
      hard_state_ = old_hard_state;
      return std::unexpected(result.error());
    }
  }

  role_ = Role::Follower;
  if (old_role == Role::Leader) {
    fail_pending_proposals(std::nullopt,
                           Error{ErrorCode::NotLeader,
                                 "leadership was lost before commit"});
    fail_read_barriers(Error{
        ErrorCode::NotLeader,
        "leadership was lost before the read barrier completed"});
  }
  leader_id_ = std::move(leader_id);
  votes_received_.clear();
  // A term change invalidates any half-received snapshot from the old leader.
  pending_snapshot_.reset();
  reset_election_deadline();
  return {};
}

std::expected<void, Error>
RaftNode::handle_request_vote(NodeId from, const RequestVoteRequest &request) {
  if (request.term < hard_state_.term) {
    transport_.send(from, RequestVoteResponse{hard_state_.term, false});
    return {};
  }
  if (request.term > hard_state_.term) {
    if (auto result = become_follower(request.term, std::nullopt);
        !result.has_value()) {
      return std::unexpected(result.error());
    }
  }

  auto last_term = last_log_term();
  if (!last_term.has_value()) {
    return std::unexpected(last_term.error());
  }

  const bool can_vote = !hard_state_.voted_for.has_value() ||
                        *hard_state_.voted_for == request.candidate_id;
  const bool log_ok =
      log_is_up_to_date(last_log_index_, *last_term,
                        request.last_log_index, request.last_log_term);
  const bool granted = can_vote && log_ok;

  if (granted) {
    const HardState old_hard_state = hard_state_;
    hard_state_.voted_for = request.candidate_id;
    if (auto result = log_store_.save_hard_state(hard_state_);
        !result.has_value()) {
      hard_state_ = old_hard_state;
      return std::unexpected(result.error());
    }
    reset_election_deadline();
  }

  transport_.send(from, RequestVoteResponse{hard_state_.term, granted});
  return {};
}

void RaftNode::handle_request_vote_response(
    NodeId from, const RequestVoteResponse &response) {
  if (response.term > hard_state_.term) {
    if (auto result = become_follower(response.term, std::nullopt);
        !result.has_value()) {
      record_error(result.error());
    }
    return;
  }
  if (role_ != Role::Candidate || response.term != hard_state_.term ||
      !response.vote_granted) {
    return;
  }

  votes_received_.insert(from);
  if (!has_quorum(votes_received_.size())) {
    return;
  }
  if (auto result = become_leader(); !result.has_value()) {
    record_error(result.error());
  }
}

std::expected<void, Error>
RaftNode::handle_append_entries(NodeId from,
                                const AppendEntriesRequest &request) {
  if (request.term < hard_state_.term) {
    transport_.send(
        from, AppendEntriesResponse{hard_state_.term, false, 0, request.round});
    return {};
  }

  if (request.term > hard_state_.term ||
      (request.term == hard_state_.term && role_ != Role::Follower)) {
    if (auto result = become_follower(request.term, request.leader_id);
        !result.has_value()) {
      return std::unexpected(result.error());
    }
  } else {
    leader_id_ = request.leader_id;
    reset_election_deadline();
  }

  if (request.prev_log_index > last_log_index_) {
    transport_.send(from, AppendEntriesResponse{hard_state_.term, false, 0,
                                                request.round, last_log_index_});
    return {};
  }

  // The referenced prefix may have been compacted into a snapshot. Terms are
  // only known at the snapshot boundary; anything below it cannot be
  // verified, so reject and let the leader fall back to a snapshot.
  if (request.prev_log_index < last_included_index_) {
    transport_.send(from, AppendEntriesResponse{hard_state_.term, false, 0,
                                                request.round, last_log_index_});
    return {};
  }

  if (request.prev_log_index != kInvalidIndex) {
    uint64_t prev_term = 0;
    if (request.prev_log_index == last_included_index_) {
      prev_term = last_included_term_;
    } else {
      auto prev = log_store_.at(request.prev_log_index);
      if (!prev.has_value()) {
        return std::unexpected(prev.error());
      }
      prev_term = prev->term;
    }
    if (prev_term != request.prev_log_term) {
      transport_.send(from, AppendEntriesResponse{hard_state_.term, false, 0,
                                                  request.round,
                                                  last_log_index_});
      return {};
    }
  }

  uint64_t index = request.prev_log_index;
  for (const auto &entry_message : request.entries) {
    const LogEntry &entry = entry_message.entry;
    ++index;

    if (entry.index != index || entry.term != request.term) {
      return std::unexpected(Error{
          ErrorCode::InvalidArgument,
          "append entries request has inconsistent index or term"});
    }

    if (index <= last_log_index_) {
      auto existing = log_store_.at(index);
      if (!existing.has_value()) {
        return std::unexpected(existing.error());
      }
      if (existing->term != entry.term) {
        if (auto result = log_store_.truncate_suffix(index);
            !result.has_value()) {
          return std::unexpected(result.error());
        }
        last_log_index_ = index - 1;
        fail_pending_proposals(
            index,
            Error{ErrorCode::NotLeader,
                  "proposal log suffix was truncated by a new leader"});
      } else if (existing->data != entry.data) {
        return std::unexpected(Error{
            ErrorCode::InternalError,
            "same log position has different data in the same term"});
      }
    }

    if (index > last_log_index_) {
      if (auto result = log_store_.append(entry); !result.has_value()) {
        return std::unexpected(result.error());
      }
      last_log_index_ = index;
    }
  }

  const uint64_t match_index = index;
  if (request.leader_commit > commit_index_) {
    commit_index_ = std::min(request.leader_commit, last_log_index_);
  }

  auto apply_result = apply_committed();
  transport_.send(
      from, AppendEntriesResponse{hard_state_.term, true, match_index,
                                  request.round});
  if (!apply_result.has_value()) {
    return std::unexpected(apply_result.error());
  }
  return {};
}

void RaftNode::handle_append_entries_response(
    NodeId from, const AppendEntriesResponse &response) {
  if (response.term > hard_state_.term) {
    if (auto result = become_follower(response.term, std::nullopt);
        !result.has_value()) {
      record_error(result.error());
    }
    return;
  }
  if (role_ != Role::Leader || response.term != hard_state_.term) {
    return;
  }

  if (response.success) {
    match_index_[from] = std::max(match_index_[from], response.match_index);
    next_index_[from] = std::max(next_index_[from],
                                  match_index_[from] + 1);
    peer_acked_round_[from] =
        std::max(peer_acked_round_[from], response.round);

    const uint64_t old_commit = commit_index_;
    if (auto result = advance_commit(); !result.has_value()) {
      record_error(result.error());
      return;
    }

    evaluate_read_barriers();

    if (match_index_[from] < last_log_index_ || commit_index_ != old_commit) {
      if (auto result = send_append_entries(from); !result.has_value()) {
        record_error(result.error());
      }
    }
    return;
  }

  if (next_index_[from] > 1) {
    // The follower reports its last log index on failure. If it is entirely
    // behind the compacted prefix, it needs a snapshot rather than a full log
    // replay (backtracking through the whole log would take forever).
    if (response.hint_last_index < last_included_index_) {
      if (auto result = send_snapshot(from); !result.has_value()) {
        record_error(result.error());
      }
      return;
    }
    --next_index_[from];
  }
  if (auto result = send_append_entries(from); !result.has_value()) {
    record_error(result.error());
  }
}

std::expected<void, Error>
RaftNode::handle_install_snapshot(NodeId from,
                                  const InstallSnapshotRequest &request) {
  if (request.term < hard_state_.term) {
    transport_.send(from, InstallSnapshotResponse{hard_state_.term, false});
    return {};
  }
  const bool term_changed = request.term > hard_state_.term;
  if (request.term > hard_state_.term ||
      (request.term == hard_state_.term && role_ != Role::Follower)) {
    if (auto result = become_follower(request.term, request.leader_id);
        !result.has_value()) {
      return std::unexpected(result.error());
    }
  } else {
    leader_id_ = request.leader_id;
    reset_election_deadline();
  }

  if (request.last_included_index == kInvalidIndex ||
      request.last_included_term == 0) {
    transport_.send(from, InstallSnapshotResponse{hard_state_.term, false});
    return {};
  }

  // A snapshot labeled behind our own applied state would revert the state
  // machine, and request-result deduplication would then skip replaying the
  // reverted entries, silently diverging. A correct leader never sends one
  // (it only snapshots peers behind its compaction point); refuse loudly and
  // let the leader retry with a newer snapshot.
  if (request.last_included_index < last_applied_) {
    transport_.send(from, InstallSnapshotResponse{hard_state_.term, false});
    return {};
  }

  // Accumulate the chunked snapshot on the raft service thread. Chunks from
  // the leader arrive in order over one connection, so no staging of
  // concurrent log entries is needed: the whole install happens atomically
  // when the final chunk arrives, before any other message is processed.
  if (request.offset == 0 || term_changed ||
      !pending_snapshot_.has_value()) {
    pending_snapshot_ = PendingSnapshot{request.term,
                                        request.last_included_index,
                                        request.last_included_term, {}};
  }
  PendingSnapshot &pending = *pending_snapshot_;
  if (pending.last_included_index != request.last_included_index ||
      pending.last_included_term != request.last_included_term ||
      request.offset != pending.buffer.size()) {
    // Out-of-order or overlapping chunks: the follower cannot assemble the
    // snapshot. Drop it and ask the leader to resend from offset 0.
    pending_snapshot_.reset();
    transport_.send(from, InstallSnapshotResponse{hard_state_.term, false});
    return {};
  }
  pending.buffer.append(request.data);
  if (pending.buffer.size() > kMaxPendingSnapshotBytes) {
    pending_snapshot_.reset();
    transport_.send(from, InstallSnapshotResponse{hard_state_.term, false});
    return std::unexpected(Error{
        ErrorCode::InvalidArgument,
        "raft snapshot exceeds the pending-buffer size limit"});
  }

  if (!request.done) {
    return {}; // waiting for the remaining chunks
  }

  // Restore the state machine data first: if we crash before the log store is
  // compacted, a restart replays the (idempotent) log over the restored data.
  if (auto result = state_machine_.restore(snapshot_range(), pending.buffer);
      !result.has_value()) {
    pending_snapshot_.reset();
    transport_.send(from, InstallSnapshotResponse{hard_state_.term, false});
    return std::unexpected(result.error());
  }

  SnapshotMetadata meta{pending.last_included_index,
                        pending.last_included_term};
  if (auto result = log_store_.install_snapshot(meta); !result.has_value()) {
    pending_snapshot_.reset();
    transport_.send(from, InstallSnapshotResponse{hard_state_.term, false});
    return std::unexpected(result.error());
  }
  pending_snapshot_.reset();

  last_included_index_ = meta.last_included_index;
  last_included_term_ = meta.last_included_term;
  last_log_index_ = std::max(last_log_index_, meta.last_included_index);
  if (last_applied_ < meta.last_included_index) {
    last_applied_ = meta.last_included_index;
  }
  if (commit_index_ < meta.last_included_index) {
    commit_index_ = meta.last_included_index;
  }

  // Everything at or below the snapshot boundary is applied by construction.
  // A follower never proposes, so there are normally no pending proposals
  // here; complete any leftovers so a waiter can never hang.
  complete_applied_proposals_upto(meta.last_included_index);
  evaluate_read_barriers();

  transport_.send(from, InstallSnapshotResponse{hard_state_.term, true});
  return {};
}

void RaftNode::handle_install_snapshot_response(
    NodeId from, const InstallSnapshotResponse &response) {
  if (response.term > hard_state_.term) {
    if (auto result = become_follower(response.term, std::nullopt);
        !result.has_value()) {
      record_error(result.error());
    }
    return;
  }
  if (role_ != Role::Leader || response.term != hard_state_.term) {
    return;
  }
  if (response.success) {
    // send_snapshot already advanced next/match optimistically. Keep sending
    // whatever log remains after the snapshot.
    if (match_index_[from] < last_log_index_) {
      if (auto result = send_append_entries(from); !result.has_value()) {
        record_error(result.error());
      }
    }
    return;
  }
  // The follower failed to install (e.g. a restore error). Leave progress as
  // it is; the next heartbeat retries the snapshot.
  record_error(Error{ErrorCode::InternalError,
                     "peer failed to install the raft snapshot"});
}

std::expected<void, Error> RaftNode::send_append_entries(NodeId to) {
  if (!next_index_.contains(to)) {
    next_index_[to] = last_log_index_ + 1;
  }
  if (!match_index_.contains(to)) {
    match_index_[to] = 0;
  }

  const uint64_t next = next_index_[to];
  // The entry the follower needs has been compacted into a snapshot: hand it
  // the snapshot instead of a log replay.
  if (next <= last_included_index_) {
    return send_snapshot(to);
  }

  const uint64_t prev_index = next - 1;
  uint64_t prev_term = 0;
  if (prev_index == last_included_index_) {
    prev_term = last_included_term_;
  } else if (prev_index != kInvalidIndex) {
    auto prev = log_store_.at(prev_index);
    if (!prev.has_value()) {
      return std::unexpected(prev.error());
    }
    prev_term = prev->term;
  }

  AppendEntriesRequest request;
  request.term = hard_state_.term;
  request.leader_id = config_.node_id;
  request.prev_log_index = prev_index;
  request.prev_log_term = prev_term;
  request.leader_commit = commit_index_;

  // Carry the round so the response can be credited to exactly the round that
  // produced it; retries reuse the round of their batch.
  request.round = heartbeat_round_;

  for (uint64_t index = next; index <= last_log_index_; ++index) {
    auto entry = log_store_.at(index);
    if (!entry.has_value()) {
      return std::unexpected(entry.error());
    }
    request.entries.push_back(LogEntryMessage{*entry});
  }

  transport_.send(to, request);
  return {};
}

std::expected<void, Error> RaftNode::send_snapshot(NodeId to) {
  if (last_included_index_ == kInvalidIndex) {
    return {}; // nothing compacted yet; the caller should not reach here
  }
  auto data = state_machine_.snapshot(snapshot_range());
  if (!data.has_value()) {
    return std::unexpected(data.error());
  }

  // The snapshot is the *current* state machine state, so label it with the
  // index it actually covers: last_applied_. This keeps the blob and its
  // metadata consistent even when this node applied entries since its own
  // compaction point. The follower jumps straight to the applied index.
  const uint64_t included_index = last_applied_;
  uint64_t included_term = 0;
  if (included_index == last_included_index_) {
    included_term = last_included_term_;
  } else {
    auto entry = log_store_.at(included_index);
    if (!entry.has_value()) {
      return std::unexpected(entry.error());
    }
    included_term = entry->term;
  }

  // Split the snapshot into ordered chunks. They are enqueued in one call, so
  // they stay in order on the outbound queue; the transport preserves
  // per-connection ordering, so the follower assembles them in order.
  uint64_t offset = 0;
  const std::string &blob = *data;
  while (offset < blob.size()) {
    const size_t take =
        std::min(kSnapshotChunkBytes, static_cast<size_t>(blob.size() - offset));
    InstallSnapshotRequest request;
    request.term = hard_state_.term;
    request.leader_id = config_.node_id;
    request.last_included_index = included_index;
    request.last_included_term = included_term;
    request.offset = offset;
    request.done = (offset + take == blob.size());
    request.data = blob.substr(offset, take);
    transport_.send(to, request);
    offset += take;
  }
  if (blob.empty()) {
    // A degenerate empty snapshot still needs one frame so the follower knows
    // the transfer is complete.
    InstallSnapshotRequest request;
    request.term = hard_state_.term;
    request.leader_id = config_.node_id;
    request.last_included_index = included_index;
    request.last_included_term = included_term;
    request.offset = 0;
    request.done = true;
    transport_.send(to, request);
  }

  // Optimistically move the follower past the snapshot; the next
  // AppendEntries verifies and refines the progress.
  next_index_[to] = included_index + 1;
  match_index_[to] = std::max(match_index_[to], included_index);
  return {};
}

std::expected<void, Error> RaftNode::maybe_compact_log() {
  // Both leaders and up-to-date followers compact their own logs once enough
  // applied entries have accumulated. Candidates are skipped (brief anyway).
  if (role_ != Role::Leader && role_ != Role::Follower) {
    return {};
  }
  if (config_.snapshot_entries_threshold == 0) {
    return {};
  }
  const uint64_t kept = last_log_index_ - last_included_index_;
  if (kept < config_.snapshot_entries_threshold) {
    return {};
  }
  const uint64_t target = last_applied_;
  if (target == kInvalidIndex || target <= last_included_index_) {
    return {};
  }

  auto entry = log_store_.at(target);
  if (!entry.has_value()) {
    return std::unexpected(entry.error());
  }

  // Only the metadata is stored here. The snapshot blob is generated on
  // demand by send_snapshot() when a lagging peer actually needs it, so an
  // up-to-date follower never pays for a full-DB scan just to shrink its log.
  SnapshotMetadata meta{target, entry->term};
  if (auto result = log_store_.install_snapshot(meta); !result.has_value()) {
    return std::unexpected(result.error());
  }
  last_included_index_ = target;
  last_included_term_ = entry->term;
  // commit_index_ is already >= last_applied_ == target. Peers behind the
  // compacted prefix receive the snapshot on the next heartbeat.
  return {};
}

void RaftNode::send_heartbeats() {
  ++heartbeat_round_;
  for (const NodeId peer : config_.peers) {
    if (peer == config_.node_id) {
      continue;
    }
    if (auto result = send_append_entries(peer); !result.has_value()) {
      record_error(result.error());
    }
  }
}

std::expected<void, Error> RaftNode::advance_commit() {
  const uint64_t candidate = quorum_match_index();
  if (candidate <= commit_index_ || candidate > last_log_index_) {
    return {};
  }

  auto entry = log_store_.at(candidate);
  if (!entry.has_value()) {
    return std::unexpected(entry.error());
  }
  if (entry->term != hard_state_.term) {
    return {};
  }

  commit_index_ = candidate;
  if (auto result = apply_committed(); !result.has_value()) {
    return std::unexpected(result.error());
  }

  // Followers learn the new commit index from the next heartbeat.
  if (config_.peers.size() > 1) {
    send_heartbeats();
  }
  return {};
}

std::expected<void, Error> RaftNode::apply_committed() {
  while (last_applied_ < commit_index_) {
    auto entry = log_store_.at(last_applied_ + 1);
    if (!entry.has_value()) {
      return std::unexpected(entry.error());
    }
    const auto apply_start = std::chrono::steady_clock::now();
    auto result = state_machine_.apply(*entry);
    const auto apply_ns = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now() - apply_start)
            .count());
    ++apply_count_;
    total_apply_ns_ += apply_ns;
    if (apply_ns > max_apply_ns_) {
      max_apply_ns_ = apply_ns;
    }
    if (!result.has_value()) {
      fail_pending_proposals(last_applied_ + 1, result.error());
      fail_read_barriers(result.error());
      return std::unexpected(result.error());
    }
    ++last_applied_;
    complete_applied_proposal(last_applied_, *result);
  }
  evaluate_read_barriers();
  return {};
}

void RaftNode::complete_applied_proposal(
    uint64_t index, const std::string &apply_result) {
  const auto it = pending_proposals_.find(index);
  if (it == pending_proposals_.end()) {
    return;
  }
  auto completion = std::move(it->second);
  pending_proposals_.erase(it);
  completion->finish(std::nullopt, apply_result);
}

void RaftNode::complete_applied_proposals_upto(uint64_t index) {
  std::vector<std::shared_ptr<ProposalCompletion>> completions;
  for (auto it = pending_proposals_.begin();
       it != pending_proposals_.end();) {
    if (it->first <= index) {
      completions.push_back(std::move(it->second));
      it = pending_proposals_.erase(it);
    } else {
      ++it;
    }
  }
  for (auto &completion : completions) {
    // Defensive fallback: the entry's effect is inside the snapshot, so it
    // was applied when the snapshot was generated. The current state machine
    // reports the constant "applied" result for every proposal.
    completion->finish(std::nullopt, "applied");
  }
}

void RaftNode::fail_pending_proposals(std::optional<uint64_t> from_index,
                                      Error error) {
  std::vector<std::shared_ptr<ProposalCompletion>> completions;
  for (auto it = pending_proposals_.begin();
       it != pending_proposals_.end();) {
    if (!from_index.has_value() || it->first >= *from_index) {
      completions.push_back(std::move(it->second));
      it = pending_proposals_.erase(it);
    } else {
      ++it;
    }
  }
  for (auto &completion : completions) {
    completion->finish(error, {});
  }
}

void RaftNode::evaluate_read_barriers() {
  for (auto it = pending_read_barriers_.begin();
       it != pending_read_barriers_.end();) {
    if (quorum_acknowledged(it->round) && last_applied_ >= it->target_index) {
      auto completion = std::move(it->completion);
      it = pending_read_barriers_.erase(it);
      completion->finish(std::nullopt, {});
    } else {
      ++it;
    }
  }
}

void RaftNode::fail_read_barriers(Error error) {
  std::vector<std::shared_ptr<Completion>> completions;
  completions.reserve(pending_read_barriers_.size());
  for (auto &entry : pending_read_barriers_) {
    completions.push_back(std::move(entry.completion));
  }
  pending_read_barriers_.clear();
  for (auto &completion : completions) {
    completion->finish(error, {});
  }
}

bool RaftNode::quorum_acknowledged(uint64_t round) const {
  size_t acknowledged = 1; // The leader itself is always up to date.
  for (const NodeId peer : config_.peers) {
    if (peer == config_.node_id) {
      continue;
    }
    const auto acked = peer_acked_round_.find(peer);
    if (acked != peer_acked_round_.end() && acked->second >= round) {
      ++acknowledged;
    }
  }
  return has_quorum(acknowledged);
}

kv::KeyRange RaftNode::snapshot_range() const {
  kv::KeyRange range = config_.group_range;
  if (!range.start.has_value()) {
    // P1 single group covers the whole key space: scan from the very first
    // key to the end.
    range.start = std::string{};
  }
  if (range.direction != kv::ScanDirection::kForward) {
    range.direction = kv::ScanDirection::kForward;
  }
  return range;
}

std::expected<uint64_t, Error> RaftNode::last_log_term() const {
  if (last_log_index_ == kInvalidIndex) {
    return 0;
  }
  auto entry = log_store_.at(last_log_index_);
  if (!entry.has_value()) {
    return std::unexpected(entry.error());
  }
  return entry->term;
}

void RaftNode::reset_election_deadline() {
  // A deterministic offset breaks ties in tests and avoids every node
  // campaigning at exactly the same logical millisecond.
  const uint64_t offset =
      (config_.node_id.value % 5) * (config_.election_timeout_ms / 10);
  election_deadline_ = clock_.now_ms() + config_.election_timeout_ms + offset;
}

void RaftNode::record_error(Error error) { last_error_ = std::move(error); }

uint64_t RaftNode::quorum_match_index() const {
  std::vector<uint64_t> matches;
  matches.reserve(config_.peers.size());
  for (const NodeId peer : config_.peers) {
    const auto it = match_index_.find(peer);
    matches.push_back(it == match_index_.end() ? 0 : it->second);
  }
  std::sort(matches.begin(), matches.end(), std::greater<uint64_t>());
  if (matches.empty()) {
    return kInvalidIndex;
  }
  return matches[(matches.size() - 1) / 2];
}

bool RaftNode::has_quorum(size_t count) const {
  return count >= (config_.peers.size() / 2) + 1;
}

void RaftNode::initialize_leader_progress() {
  next_index_.clear();
  match_index_.clear();
  peer_acked_round_.clear();
  heartbeat_round_ = 0;
  for (const NodeId peer : config_.peers) {
    next_index_[peer] = last_log_index_ + 1;
    match_index_[peer] = peer == config_.node_id ? last_log_index_ : 0;
  }
}

} // namespace raft
