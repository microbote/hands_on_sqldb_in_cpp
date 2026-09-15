#pragma once

#include <cstddef>
#include <cstdint>
#include <expected>
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

  static bool is_system_key(const kv::Key &key) {
    return key.starts_with(kSystemPrefix);
  }

  // Adds a data range owned by `group_id`. Ranges may overlap; group_for()
  // picks the first matching range (undefined behavior is avoided in practice
  // by keeping the ranges disjoint in configuration).
  void add_range(kv::Key start, kv::Key end, uint64_t group_id);

  // Validates the routing table: disjoint ranges, data groups >= 1 and unique,
  // and no data range covering any "@system/" key (group 0's reserved space).
  // Returns a human-readable problem, or nullopt when the table is valid.
  std::optional<std::string> validate() const;

  uint64_t group_for(const kv::Key &key) const;

  // The single group owning the whole [start, end) range, or nullopt when the
  // range spans more than one group. A nullopt `end` means "to the end".
  std::optional<uint64_t>
  range_group(const kv::Key &start, const std::optional<kv::Key> &end) const;

  // The single group owning every op of a batch, or nullopt when the batch
  // would cross groups (a write that must be rejected).
  std::optional<uint64_t> batch_group(const kv::WriteBatch &batch) const;

  size_t group_count() const;

  // The key range owned by a data group, or nullopt for group 0 (which owns
  // @system/* plus everything not covered by a data range — not a single
  // contiguous range). Used for per-group snapshot ranges.
  std::optional<kv::KeyRange> group_range(uint64_t group_id) const;

  // Parses the `[raft] shards` text:
  //   "<start>,<end>,<group_id>; <start>,<end>,<group_id>; ..."
  // Each entry adds a data range [start, end) owned by group_id (>= 1).
  // The whole table is validated before returning. Keys must not contain ',' or
  // ';' (the table-key encoding uses ':' and '/', so this holds in practice).
  static std::expected<GroupRouter, std::string>
  parse_shards(std::string_view text);

private:
  struct Range {
    kv::Key start;
    kv::Key end;
    uint64_t group_id = 0;
  };
  std::vector<Range> ranges_; // sorted by start
};

} // namespace raft
