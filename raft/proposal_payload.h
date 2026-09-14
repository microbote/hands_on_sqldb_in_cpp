#pragma once

#include <cstdint>
#include <expected>
#include <string>
#include <string_view>

#include "raft/types.h"
#include "storage/kv_engine/kv_engine.h"

namespace raft {

struct ProposalPayload {
  uint64_t client_id = 0;
  uint64_t request_id = 0;
  kv::WriteBatch batch;
};

// Binary-safe encoding for a Raft log entry's data field.
//
// Layout (all integers are big-endian):
//   magic       4 bytes  "SQRA"
//   version     u32
//   client_id   u64
//   request_id  u64
//   op_count    u64
//   op:
//     type      u8
//     key_len   u64 + bytes
//     has_value u8
//     value_len u64 + bytes (only when has_value)
//     end_len   u64 + bytes
//
// The sync flag is intentionally not encoded: durability is a local LogStore
// policy, not replicated state.
std::string encode_proposal_payload(const ProposalPayload &payload);

std::expected<ProposalPayload, Error>
decode_proposal_payload(std::string_view data);

} // namespace raft
