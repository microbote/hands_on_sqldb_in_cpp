#include "raft/raft_runtime.h"

#include <condition_variable>
#include <memory>
#include <mutex>
#include <optional>
#include <utility>

namespace raft {
namespace {

// Shared with the queued work item: the caller waits on it, the service thread
// signals it. `done` is written under the mutex, so everything the work item
// touched before signaling is visible to the caller afterwards.
struct SubmitState {
  std::mutex mutex;
  std::condition_variable condition;
  bool done = false;
};

} // namespace

RaftRuntime::RaftRuntime(RaftNode &node, std::string name)
    : node_(node), service_(std::move(name)) {}

RaftRuntime::~RaftRuntime() { stop(); }

void RaftRuntime::start(size_t queue_max) {
  if (running_.load()) {
    return;
  }
  service_.start(queue_max);
  running_.store(true);

  // Publish this thread's id before any outside caller can check affinity.
  auto state = std::make_shared<SubmitState>();
  const bool queued = service_.submit(
      [this, state] {
        worker_id_.store(std::this_thread::get_id());
        {
          std::lock_guard<std::mutex> lock(state->mutex);
          state->done = true;
        }
        state->condition.notify_all();
      },
      nullptr, {});
  if (!queued) {
    running_.store(false);
    service_.stop();
    return;
  }
  std::unique_lock<std::mutex> lock(state->mutex);
  state->condition.wait(lock, [&] { return state->done; });
}

void RaftRuntime::stop() {
  running_.store(false);
  // ServiceThread::stop() drains the queue before returning, so a queued tick
  // or message is applied even during shutdown.
  service_.stop();
}

bool RaftRuntime::submit_blocking(std::function<void()> work, Error *error) {
  if (on_service_thread()) {
    *error = Error{
        ErrorCode::Busy,
        "blocking raft calls are not allowed on the raft service thread"};
    return false;
  }
  if (!ready()) {
    *error = Error{ErrorCode::InternalError, "raft runtime is not running"};
    return false;
  }

  auto state = std::make_shared<SubmitState>();
  const bool queued = service_.submit(
      [state, work = std::move(work)]() mutable {
        work();
        {
          std::lock_guard<std::mutex> lock(state->mutex);
          state->done = true;
        }
        state->condition.notify_all();
      },
      nullptr, {});
  if (!queued) {
    rejected_.fetch_add(1);
    *error = Error{ErrorCode::Busy, "raft service queue is full"};
    return false;
  }

  std::unique_lock<std::mutex> lock(state->mutex);
  state->condition.wait(lock, [&] { return state->done; });
  return true;
}

std::expected<void, Error> RaftRuntime::run(std::function<void()> fn) {
  Error error;
  if (!submit_blocking(std::move(fn), &error)) {
    return std::unexpected(error);
  }
  return {};
}

bool RaftRuntime::post(std::function<void()> work) {
  const bool queued = service_.submit(std::move(work), nullptr, {});
  if (!queued) {
    rejected_.fetch_add(1);
  }
  return queued;
}

std::expected<Proposal, Error> RaftRuntime::propose(std::string data) {
  std::optional<std::expected<Proposal, Error>> result;
  Error error;
  // The lambda owns `data`; `result` lives on the caller's stack and the
  // caller only reads it after the completion signal.
  if (!submit_blocking(
          [&, data = std::move(data)]() mutable {
            result = node_.propose(std::move(data));
          },
          &error)) {
    return std::unexpected(error);
  }
  return std::move(*result);
}

std::expected<ReadIndex, Error> RaftRuntime::read_barrier() {
  std::optional<std::expected<ReadIndex, Error>> result;
  Error error;
  if (!submit_blocking([&] { result = node_.read_barrier(); }, &error)) {
    return std::unexpected(error);
  }
  return std::move(*result);
}

uint64_t RaftRuntime::election_timeout_ms() const {
  // Configuration, not mutable Raft state: reading it off the node directly is
  // safe from any thread.
  return node_.election_timeout_ms();
}

std::optional<NodeId> RaftRuntime::leader_hint() {
  std::optional<NodeId> hint;
  if (!run([&] { hint = node_.leader_hint(); }).has_value()) {
    return std::nullopt;
  }
  return hint;
}

bool RaftRuntime::post_message(NodeId from, const Message &message) {
  const bool queued = service_.submit(
      [this, from, message] { node_.handle_message(from, message); }, nullptr,
      {});
  if (!queued) {
    rejected_.fetch_add(1);
  }
  return queued;
}

bool RaftRuntime::request_tick() {
  const bool queued = service_.submit([this] { node_.tick(); }, nullptr, {});
  if (!queued) {
    dropped_ticks_.fetch_add(1);
  }
  return queued;
}

} // namespace raft
