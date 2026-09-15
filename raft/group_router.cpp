#include "raft/group_router.h"

#include <algorithm>

namespace raft {
namespace {

bool is_system_key(const kv::Key &key) {
  return key.starts_with(GroupRouter::kSystemPrefix);
}

} // namespace

void GroupRouter::add_range(kv::Key start, kv::Key end, uint64_t group_id) {
  const auto it = std::upper_bound(
      ranges_.begin(), ranges_.end(), start,
      [](const kv::Key &key, const Range &range) { return key < range.start; });
  ranges_.insert(it, Range{std::move(start), std::move(end), group_id});
}

uint64_t GroupRouter::group_for(const kv::Key &key) const {
  if (is_system_key(key)) {
    return 0;
  }
  for (const Range &range : ranges_) {
    if (key < range.start) {
      break;
    }
    if (key < range.end) {
      return range.group_id;
    }
  }
  return 0; // default group
}

std::optional<uint64_t>
GroupRouter::range_group(const kv::Key &start,
                         const std::optional<kv::Key> &end) const {
  if (end.has_value() && *end <= start) {
    return group_for(start); // empty range: nothing to span
  }
  const uint64_t first = group_for(start);
  const auto crosses = [&](const kv::Key &boundary) {
    if (boundary <= start) {
      return false;
    }
    if (end.has_value() && boundary >= *end) {
      return false;
    }
    return group_for(boundary) != first;
  };
  if (crosses(kv::Key{GroupRouter::kSystemPrefix})) {
    return std::nullopt;
  }
  for (const Range &range : ranges_) {
    if (crosses(range.start) || crosses(range.end)) {
      return std::nullopt;
    }
  }
  return first;
}

std::optional<uint64_t>
GroupRouter::batch_group(const kv::WriteBatch &batch) const {
  std::optional<uint64_t> group;
  for (const auto &op : batch.ops()) {
    std::optional<uint64_t> op_group;
    if (op.type == kv::WriteBatch::OpType::kRemoveRange) {
      op_group = range_group(op.data.key, op.range_end);
    } else {
      op_group = group_for(op.data.key);
    }
    if (!op_group.has_value()) {
      return std::nullopt;
    }
    if (group.has_value() && *group != *op_group) {
      return std::nullopt;
    }
    group = op_group;
  }
  return group;
}

size_t GroupRouter::group_count() const {
  size_t max_group = 0;
  for (const Range &range : ranges_) {
    if (range.group_id > max_group) {
      max_group = range.group_id;
    }
  }
  return max_group + 1;
}

} // namespace raft
