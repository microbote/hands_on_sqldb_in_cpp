#pragma once

#include <expected>
#include <string>
#include <string_view>
#include <vector>

#include "raft/types.h"

namespace raft {

// Parses the static peer list used by `[raft] peers`.
//
//   "1@127.0.0.1:5434,2@127.0.0.1:5435"
//
// Rules: entry = `<node_id>@<host>:<port>`; node ids and endpoints must be
// unique; node ids are >= 1; ports are in [1, 65535]. Whitespace around
// entries is ignored, so a config file may wrap the list across lines.
// Returns a human-readable message on failure (config validation reports it
// verbatim).
std::expected<std::vector<PeerConfig>, std::string>
parse_peer_list(std::string_view text);

// Renders the list back in the canonical config form (used for logs).
std::string format_peer_list(const std::vector<PeerConfig> &peers);

} // namespace raft
