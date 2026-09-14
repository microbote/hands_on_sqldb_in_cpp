#pragma once

#include <cstdint>
#include <chrono>
#include <condition_variable>
#include <expected>
#include <memory>
#include <mutex>
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
  Busy,
  Timeout,
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

// A static member of the group: identity plus where to reach it.
struct PeerConfig {
  NodeId node_id;
  std::string host;
  uint16_t port = 0;
};

class Completion {
public:
  void finish(std::optional<Error> error, std::string result) {
    std::unique_lock<std::mutex> lock(mutex_);
    if (done_) {
      return;
    }
    done_ = true;
    error_ = std::move(error);
    result_ = std::move(result);
    lock.unlock();
    condition_.notify_all();
  }

  bool done() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return done_;
  }

  std::optional<Error> error() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return error_;
  }

private:
  friend struct Proposal;
  friend struct ReadIndex;

  mutable std::mutex mutex_;
  std::condition_variable condition_;
  bool done_ = false;
  std::optional<Error> error_;
  std::string result_;
};

using ProposalCompletion = Completion;

struct Proposal {
  uint64_t index = kInvalidIndex;
  uint64_t term = 0;
  std::string data;
  bool committed = false;
  std::string apply_result;
  std::shared_ptr<ProposalCompletion> completion;

  bool done() const {
    return completion != nullptr && completion->done();
  }

  std::expected<Proposal, Error> wait() {
    if (completion == nullptr) {
      return std::unexpected(Error{
          ErrorCode::InternalError,
          "proposal has no completion state"});
    }
    std::unique_lock<std::mutex> lock(completion->mutex_);
    completion->condition_.wait(
        lock, [this] { return completion->done_; });
    if (completion->error_.has_value()) {
      return std::unexpected(*completion->error_);
    }
    Proposal completed = *this;
    completed.committed = true;
    completed.apply_result = completion->result_;
    return completed;
  }

  template <typename Rep, typename Period>
  std::expected<Proposal, Error>
  wait_for(std::chrono::duration<Rep, Period> timeout) {
    if (completion == nullptr) {
      return std::unexpected(Error{
          ErrorCode::InternalError,
          "proposal has no completion state"});
    }
    std::unique_lock<std::mutex> lock(completion->mutex_);
    if (!completion->condition_.wait_for(
            lock, timeout, [this] { return completion->done_; })) {
      return std::unexpected(
          Error{ErrorCode::Timeout, "proposal wait timed out"});
    }
    if (completion->error_.has_value()) {
      return std::unexpected(*completion->error_);
    }
    Proposal completed = *this;
    completed.committed = true;
    completed.apply_result = completion->result_;
    return completed;
  }
};

struct ReadIndex {
  uint64_t index = kInvalidIndex;
  uint64_t term = 0;
  std::shared_ptr<Completion> completion;

  bool done() const {
    return completion != nullptr && completion->done();
  }

  std::expected<ReadIndex, Error> wait() {
    if (completion == nullptr) {
      return std::unexpected(Error{
          ErrorCode::InternalError,
          "read index has no completion state"});
    }
    std::unique_lock<std::mutex> lock(completion->mutex_);
    completion->condition_.wait(
        lock, [this] { return completion->done_; });
    if (completion->error_.has_value()) {
      return std::unexpected(*completion->error_);
    }
    return *this;
  }

  template <typename Rep, typename Period>
  std::expected<ReadIndex, Error>
  wait_for(std::chrono::duration<Rep, Period> timeout) {
    if (completion == nullptr) {
      return std::unexpected(Error{
          ErrorCode::InternalError,
          "read index has no completion state"});
    }
    std::unique_lock<std::mutex> lock(completion->mutex_);
    if (!completion->condition_.wait_for(
            lock, timeout, [this] { return completion->done_; })) {
      return std::unexpected(
          Error{ErrorCode::Timeout, "read index wait timed out"});
    }
    if (completion->error_.has_value()) {
      return std::unexpected(*completion->error_);
    }
    return *this;
  }
};

inline const char *error_code_to_string(ErrorCode code) {
  switch (code) {
  case ErrorCode::InvalidArgument:
    return "InvalidArgument";
  case ErrorCode::NotLeader:
    return "NotLeader";
  case ErrorCode::Busy:
    return "Busy";
  case ErrorCode::Timeout:
    return "Timeout";
  case ErrorCode::InternalError:
    return "InternalError";
  case ErrorCode::IOError:
    return "IOError";
  }
  return "Unknown";
}

} // namespace raft
