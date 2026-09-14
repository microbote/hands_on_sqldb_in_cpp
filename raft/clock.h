#pragma once

#include <chrono>
#include <cstdint>

namespace raft {

// RaftNode only reads logical time through this interface. Tests inject a
// manual clock; the production adapter will use the event-loop time source.
class Clock {
public:
  virtual ~Clock() = default;
  virtual uint64_t now_ms() const = 0;
};

// Production clock: monotonic (steady) milliseconds. Elections and heartbeats
// only need a monotonic source, and steady_clock cannot jump backwards when
// the wall clock is adjusted.
class SystemClock final : public Clock {
public:
  uint64_t now_ms() const override {
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch())
            .count());
  }
};

} // namespace raft
