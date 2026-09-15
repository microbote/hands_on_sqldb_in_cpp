#include "raft/group_router.h"

#include <algorithm>
#include <cstdint>
#include <set>
#include <string_view>
#include <utility>

namespace raft {
namespace {

bool is_system_key(const kv::Key &key) {
  return key.starts_with(GroupRouter::kSystemPrefix);
}

std::string_view trim(std::string_view text) {
  size_t begin = 0;
  while (begin < text.size() &&
         (text[begin] == ' ' || text[begin] == '\t' || text[begin] == '\n' ||
          text[begin] == '\r')) {
    ++begin;
  }
  size_t end = text.size();
  while (end > begin && (text[end - 1] == ' ' || text[end - 1] == '\t' ||
                         text[end - 1] == '\n' || text[end - 1] == '\r')) {
    --end;
  }
  return text.substr(begin, end - begin);
}

bool parse_u64(std::string_view text, uint64_t *out) {
  if (text.empty()) {
    return false;
  }
  uint64_t value = 0;
  for (const char c : text) {
    if (c < '0' || c > '9') {
      return false;
    }
    value = value * 10 + static_cast<uint64_t>(c - '0');
  }
  *out = value;
  return true;
}

} // namespace

void GroupRouter::add_range(kv::Key start, kv::Key end, uint64_t group_id) {
  const auto it = std::upper_bound(
      ranges_.begin(), ranges_.end(), start,
      [](const kv::Key &key, const Range &range) { return key < range.start; });
  ranges_.insert(it, Range{std::move(start), std::move(end), group_id});
}

std::optional<std::string> GroupRouter::validate() const {
  const kv::Key system{kSystemPrefix};
  std::set<uint64_t> data_groups;
  for (size_t i = 0; i < ranges_.size(); ++i) {
    const Range &range = ranges_[i];
    if (range.group_id == 0) {
      return "raft shards: data range for '" + range.start + "' uses group 0 "
             "(reserved for @system/*)";
    }
    if (!data_groups.insert(range.group_id).second) {
      return "raft shards: group " + std::to_string(range.group_id) +
             " appears more than once";
    }
    if (range.end <= range.start) {
      return "raft shards: range ['" + range.start + "', '" + range.end +
             "') is empty";
    }
    if (range.start.find(',') != std::string::npos ||
        range.start.find(';') != std::string::npos ||
        range.end.find(',') != std::string::npos ||
        range.end.find(';') != std::string::npos) {
      return "raft shards: keys must not contain ',' or ';'";
    }
    // A data range may not cover any @system/* key: it contains the smallest
    // system key exactly when start <= "@system/" < end (or starts inside the
    // prefix).
    if (is_system_key(range.start) ||
        (range.start < system && range.end > system)) {
      return "raft shards: range ['" + range.start + "', '" + range.end +
             "') covers @system/* which is reserved for group 0";
    }
    if (i > 0 && ranges_[i - 1].end > range.start) {
      return "raft shards: ranges overlap at '" + range.start + "'";
    }
  }
  // Data group ids must be exactly 1..ranges.size() so group_count() has no
  // holes and every group has a node.
  for (uint64_t id = 1; id <= ranges_.size(); ++id) {
    if (!data_groups.contains(id)) {
      return "raft shards: group ids must be contiguous starting at 1 (missing "
             "group " +
             std::to_string(id) + ")";
    }
  }
  return std::nullopt;
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

std::optional<kv::KeyRange> GroupRouter::group_range(uint64_t group_id) const {
  for (const Range &range : ranges_) {
    if (range.group_id == group_id) {
      return kv::KeyRange{range.start, range.end, 0,
                          kv::ScanDirection::kForward};
    }
  }
  return std::nullopt;
}

std::expected<GroupRouter, std::string>
GroupRouter::parse_shards(std::string_view text) {
  GroupRouter router;
  size_t offset = 0;
  while (offset <= text.size()) {
    const size_t semicolon = text.find(';', offset);
    const std::string_view entry =
        trim(semicolon == std::string_view::npos
                 ? text.substr(offset)
                 : text.substr(offset, semicolon - offset));
    offset = semicolon == std::string_view::npos ? text.size() + 1
                                                 : semicolon + 1;
    if (entry.empty()) {
      if (semicolon == std::string_view::npos) {
        break; // trailing ';' / whitespace is not an entry
      }
      return std::unexpected("raft shards: empty entry");
    }

    const size_t first_comma = entry.find(',');
    if (first_comma == std::string_view::npos) {
      return std::unexpected("raft shards: entry '" + std::string(entry) +
                             "' must look like <start>,<end>,<group_id>");
    }
    const size_t second_comma = entry.find(',', first_comma + 1);
    if (second_comma == std::string_view::npos ||
        entry.find(',', second_comma + 1) != std::string_view::npos) {
      return std::unexpected("raft shards: entry '" + std::string(entry) +
                             "' must look like <start>,<end>,<group_id>");
    }
    const kv::Key start{entry.substr(0, first_comma)};
    const kv::Key end{
        entry.substr(first_comma + 1, second_comma - first_comma - 1)};
    uint64_t group_id = 0;
    if (!parse_u64(trim(entry.substr(second_comma + 1)), &group_id)) {
      return std::unexpected("raft shards: entry '" + std::string(entry) +
                             "' has an invalid group id");
    }
    router.add_range(std::move(start), std::move(end), group_id);
  }

  if (const auto problem = router.validate(); problem.has_value()) {
    return std::unexpected(*problem);
  }
  return router;
}

} // namespace raft
