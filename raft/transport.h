#pragma once

#include <functional>
#include <variant>

#include "raft/types.h"

namespace raft {

struct RequestVoteRequest {
  uint64_t term = 0;
  NodeId candidate_id;
  uint64_t last_log_index = kInvalidIndex;
  uint64_t last_log_term = 0;
};

struct RequestVoteResponse {
  uint64_t term = 0;
  bool vote_granted = false;
};

struct LogEntryMessage {
  LogEntry entry;
};

struct AppendEntriesRequest {
  uint64_t term = 0;
  NodeId leader_id;
  uint64_t prev_log_index = kInvalidIndex;
  uint64_t prev_log_term = 0;
  std::vector<LogEntryMessage> entries;
  uint64_t leader_commit = kInvalidIndex;
  // Heartbeat round this request belongs to, echoed back in the response so
  // that read-index confirmation can only be credited to the round that
  // actually produced it. 0 means "no read-index round".
  uint64_t round = 0;
};

struct AppendEntriesResponse {
  uint64_t term = 0;
  bool success = false;
  uint64_t match_index = kInvalidIndex;
  uint64_t round = 0;
  // On failure, the follower's last log index. The leader uses it to detect a
  // follower that is entirely behind the compacted snapshot prefix and should
  // receive a snapshot instead of a full log replay.
  uint64_t hint_last_index = kInvalidIndex;
};

struct InstallSnapshotRequest {
  uint64_t term = 0;
  NodeId leader_id;
  uint64_t last_included_index = kInvalidIndex;
  uint64_t last_included_term = 0;
  // Byte offset of this chunk within the serialized snapshot. The first
  // chunk of a snapshot always has offset 0.
  uint64_t offset = 0;
  // True when this is the last chunk; the follower installs only then.
  bool done = true;
  // One chunk of the serialized state machine snapshot covering
  // [start, last_included_index].
  std::string data;
};

struct InstallSnapshotResponse {
  uint64_t term = 0;
  bool success = false;
};

using Message = std::variant<RequestVoteRequest, RequestVoteResponse,
                             AppendEntriesRequest, AppendEntriesResponse,
                             InstallSnapshotRequest, InstallSnapshotResponse>;

// Transport is asynchronous from RaftNode's perspective. Implementations must
// queue send() calls rather than synchronously re-entering RaftNode.
//
// A transport may be shared by several RaftNodes (one per group): every
// message carries the sender's group id, and on_message() registers per group.
class Transport {
public:
  virtual ~Transport() = default;

  virtual void send(NodeId to, uint64_t group_id, const Message &message) = 0;
  virtual void on_message(
      uint64_t group_id,
      std::function<void(NodeId /*from*/, const Message &)> callback) = 0;
};

} // namespace raft
