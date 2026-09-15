#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "storage/kv_engine/kv_engine.h"

namespace raft {

// Static key -> group routing table (the P2 adapter slice).
//
// Rules:
//   - the "@system/" prefix is always owned by group 0;
//   - data ranges are [start, end) -> group_id, kept sorted by start;
//   - anything not covered by a data range falls back to group 0.
//
// A single-group deployment simply never calls add_range(): every key lands in
// group 0, which is exactly the pre-multi-group behavior.
class GroupRouter {
public:
  static constexpr std::string_view kSystemPrefix = "@system/";

  // Adds a data range owned by `group_id`. Ranges may overlap; group_for()
  // picks the first matching range (undefined behavior is avoided in practice
  // by keeping the ranges disjoint in configuration).
  void add_range(kv::Key start, kv::Key end, uint64_t group_id);

  uint64_t group_for(const kv::Key &key) const;

  // The single group owning the whole [start, end) range, or nullopt when the
  // range spans more than one group. A nullopt `end` means "to the end".
  std::optional<uint64_t>
  range_group(const kv::Key &start, const std::optional<kv::Key> &end) const;

  // The single group owning every op of a batch, or nullopt when the batch
  // would cross groups (a write that must be rejected).
  std::optional<uint64_t> batch_group(const kv::WriteBatch &batch) const;

  size_t group_count() const;

private:
  struct Range {
    kv::Key start;
    kv::Key end;
    uint64_t group_id = 0;
  };
  std::vector<Range> ranges_; // sorted by start
};

} // namespace raft
