#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace raft {

inline constexpr uint64_t kInvalidIndex = 0;

struct NodeId {
  uint64_t value = 0;

  friend bool operator==(const NodeId &lhs, const NodeId &rhs) {
    return lhs.value == rhs.value;
  }
  friend bool operator!=(const NodeId &lhs, const NodeId &rhs) {
    return !(lhs == rhs);
  }
  friend bool operator<(const NodeId &lhs, const NodeId &rhs) {
    return lhs.value < rhs.value;
  }
};

enum class Role {
  Follower,
  Candidate,
  Leader,
};

struct LogEntry {
  uint64_t index = kInvalidIndex;
  uint64_t term = 0;
  std::string data;
};

struct HardState {
  uint64_t term = 0;
  std::optional<NodeId> voted_for;
};

enum class ErrorCode {
  InvalidArgument,
  NotLeader,
  InternalError,
  IOError,
};

struct Error {
  ErrorCode code = ErrorCode::InternalError;
  std::string message;
};

struct NodeConfig {
  NodeId node_id;
  std::vector<NodeId> peers; // voter set; includes node_id itself.
  uint64_t election_timeout_ms = 1000;
  uint64_t heartbeat_interval_ms = 100;
};

struct Proposal {
  uint64_t index = kInvalidIndex;
  uint64_t term = 0;
  std::string data;
  bool committed = false;
};

inline const char *error_code_to_string(ErrorCode code) {
  switch (code) {
  case ErrorCode::InvalidArgument:
    return "InvalidArgument";
  case ErrorCode::NotLeader:
    return "NotLeader";
  case ErrorCode::InternalError:
    return "InternalError";
  case ErrorCode::IOError:
    return "IOError";
  }
  return "Unknown";
}

} // namespace raft
