#pragma once

#include <cstdint>

namespace raft {

// RaftNode only reads logical time through this interface. Tests inject a
// manual clock; the production adapter will use the event-loop time source.
class Clock {
public:
  virtual ~Clock() = default;
  virtual uint64_t now_ms() const = 0;
};

} // namespace raft
