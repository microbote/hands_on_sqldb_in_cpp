#pragma once

#include <cstddef>
#include <cstdint>
#include <expected>
#include <string>
#include <string_view>

#include "raft/transport.h"
#include "raft/types.h"

namespace raft {

// Binary, versioned encoding of the Raft RPCs. This is the wire format the
// production transport uses; the test transport passes Message values around
// directly.
//
// Frame layout (all integers big-endian):
//   u32 payload_size       (excludes this 4-byte prefix)
//   u8  version            (kMessageVersion)
//   u8  message_type
//   type-specific fields
//
// AppendEntries entries are encoded as
//   u64 count, then per entry: u64 index, u64 term, u64 data_len + bytes
// so entry data (a serialized WriteBatch) is binary safe.
inline constexpr uint8_t kMessageVersion = 1;
inline constexpr uint32_t kMaxFrameBytes = 64u * 1024u * 1024u;

enum class MessageType : uint8_t {
  kRequestVoteRequest = 1,
  kRequestVoteResponse = 2,
  kAppendEntriesRequest = 3,
  kAppendEntriesResponse = 4,
};

// Encodes a message payload without the length prefix.
std::string encode_message(const Message &message);
std::expected<Message, Error> decode_message(std::string_view payload);

// Encodes a length-prefixed frame (payload plus the u32 size prefix).
std::string encode_frame(const Message &message);
// Decodes one complete frame. The size prefix must match the remaining bytes
// exactly; a mismatch or trailing data is rejected.
std::expected<Message, Error> decode_frame(std::string_view frame);

// Generic version of the same framing, for transport-level payloads that are
// not Raft messages (the peer handshake).
std::string frame_payload(std::string_view payload);
std::expected<std::string, Error> unframe_payload(std::string_view frame);

// Incremental decoder for a byte stream: append whatever read() returned and
// pop complete frames in order. Used by the TCP transport.
class FrameDecoder {
public:
  void append(std::string_view bytes) { buffer_.append(bytes); }

  // Pops one complete frame (payload without the length prefix) into `out`.
  // Returns false when the buffer holds no complete frame yet; returns an
  // error when the size prefix is impossible (protocol violation — the caller
  // should close the connection).
  std::expected<bool, Error> next(std::string *out);

  size_t buffered_bytes() const { return buffer_.size(); }
  void clear() { buffer_.clear(); }

private:
  std::string buffer_;
  size_t consumed_ = 0;
};

} // namespace raft
