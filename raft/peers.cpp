#include "raft/peers.h"

#include <charconv>
#include <cstdint>
#include <optional>
#include <set>

namespace raft {
namespace {

std::string_view trim(std::string_view text) {
  size_t begin = 0;
  while (begin < text.size() &&
         (text[begin] == ' ' || text[begin] == '\t' ||
          text[begin] == '\n' || text[begin] == '\r')) {
    ++begin;
  }
  size_t end = text.size();
  while (end > begin && (text[end - 1] == ' ' || text[end - 1] == '\t' ||
                         text[end - 1] == '\n' || text[end - 1] == '\r')) {
    --end;
  }
  return text.substr(begin, end - begin);
}

template <typename T>
std::optional<T> parse_unsigned(std::string_view text) {
  if (text.empty()) {
    return std::nullopt;
  }
  T value = 0;
  const char *begin = text.data();
  const char *end = begin + text.size();
  const auto rc = std::from_chars(begin, end, value, 10);
  if (rc.ec != std::errc() || rc.ptr != end) {
    return std::nullopt;
  }
  return value;
}

} // namespace

std::expected<std::vector<PeerConfig>, std::string>
parse_peer_list(std::string_view text) {
  std::vector<PeerConfig> peers;
  std::set<uint64_t> ids;
  std::set<std::string> endpoints;

  size_t offset = 0;
  while (offset <= text.size()) {
    const size_t comma = text.find(',', offset);
    const std::string_view raw =
        comma == std::string_view::npos ? text.substr(offset)
                                        : text.substr(offset, comma - offset);
    offset = comma == std::string_view::npos ? text.size() + 1 : comma + 1;

    const std::string_view entry = trim(raw);
    if (entry.empty()) {
      if (comma == std::string_view::npos) {
        break; // trailing comma / trailing whitespace is not an entry
      }
      return std::unexpected("raft.peers: empty entry");
    }

    const size_t at = entry.find('@');
    if (at == std::string_view::npos) {
      return std::unexpected(
          "raft.peers: entry '" + std::string(entry) +
          "' must look like <node_id>@<host>:<port>");
    }
    const size_t colon = entry.rfind(':');
    if (colon == std::string_view::npos || colon < at) {
      return std::unexpected(
          "raft.peers: entry '" + std::string(entry) +
          "' must look like <node_id>@<host>:<port>");
    }

    const auto id = parse_unsigned<uint64_t>(entry.substr(0, at));
    if (!id.has_value() || *id == 0) {
      return std::unexpected(
          "raft.peers: entry '" + std::string(entry) +
          "' has an invalid node id (must be a positive integer)");
    }
    const std::string host(entry.substr(at + 1, colon - at - 1));
    if (host.empty()) {
      return std::unexpected(
          "raft.peers: entry '" + std::string(entry) +
          "' has an empty host");
    }
    const auto port = parse_unsigned<uint32_t>(entry.substr(colon + 1));
    if (!port.has_value() || *port == 0 || *port > 65535) {
      return std::unexpected(
          "raft.peers: entry '" + std::string(entry) +
          "' has an invalid port (must be in [1, 65535])");
    }

    if (!ids.insert(*id).second) {
      return std::unexpected("raft.peers: duplicate node id " +
                             std::to_string(*id));
    }
    const std::string endpoint = host + ":" + std::to_string(*port);
    if (!endpoints.insert(endpoint).second) {
      return std::unexpected("raft.peers: duplicate endpoint " + endpoint);
    }

    peers.push_back(PeerConfig{NodeId{*id}, host,
                               static_cast<uint16_t>(*port)});
  }

  if (peers.empty()) {
    return std::unexpected("raft.peers: must not be empty");
  }
  return peers;
}

std::string format_peer_list(const std::vector<PeerConfig> &peers) {
  std::string result;
  for (const auto &peer : peers) {
    if (!result.empty()) {
      result += ",";
    }
    result += std::to_string(peer.node_id.value) + "@" + peer.host + ":" +
              std::to_string(peer.port);
  }
  return result;
}

} // namespace raft
