#include "raft/message_codec.h"

#include <type_traits>
#include <utility>
#include <variant>

namespace raft {
namespace {

void put_u8(std::string &out, uint8_t value) {
  out.push_back(static_cast<char>(value));
}

void put_u32(std::string &out, uint32_t value) {
  for (int shift = 24; shift >= 0; shift -= 8) {
    put_u8(out, static_cast<uint8_t>((value >> shift) & 0xff));
  }
}

void put_u64(std::string &out, uint64_t value) {
  for (int shift = 56; shift >= 0; shift -= 8) {
    put_u8(out, static_cast<uint8_t>((value >> shift) & 0xff));
  }
}

void put_bytes(std::string &out, std::string_view value) {
  put_u64(out, value.size());
  out.append(value.data(), value.size());
}

Error truncated(std::string_view what) {
  return Error{ErrorCode::InvalidArgument,
               "truncated raft message while reading " + std::string(what)};
}

Error malformed(std::string_view what) {
  return Error{ErrorCode::InvalidArgument,
               "malformed raft message: " + std::string(what)};
}

std::expected<uint8_t, Error> read_u8(std::string_view data, size_t &offset) {
  if (data.size() < offset + 1) {
    return std::unexpected(truncated("u8"));
  }
  return static_cast<uint8_t>(data[offset++]);
}

std::expected<uint64_t, Error> read_u64(std::string_view data,
                                        size_t &offset) {
  if (data.size() < offset + sizeof(uint64_t)) {
    return std::unexpected(truncated("u64"));
  }
  uint64_t result = 0;
  for (size_t i = 0; i < sizeof(uint64_t); ++i) {
    result = (result << 8) |
             static_cast<uint64_t>(
                 static_cast<unsigned char>(data[offset + i]));
  }
  offset += sizeof(uint64_t);
  return result;
}

std::expected<uint32_t, Error> read_u32(std::string_view data,
                                        size_t &offset) {
  if (data.size() < offset + sizeof(uint32_t)) {
    return std::unexpected(truncated("u32"));
  }
  uint32_t result = 0;
  for (size_t i = 0; i < sizeof(uint32_t); ++i) {
    result = (result << 8) |
             static_cast<uint32_t>(
                 static_cast<unsigned char>(data[offset + i]));
  }
  offset += sizeof(uint32_t);
  return result;
}

std::expected<std::string, Error> read_bytes(std::string_view data,
                                             size_t &offset) {
  auto size = read_u64(data, offset);
  if (!size.has_value()) {
    return std::unexpected(size.error());
  }
  if (*size > data.size() - offset) {
    return std::unexpected(truncated("byte string"));
  }
  std::string result{data.substr(offset, *size)};
  offset += *size;
  return result;
}

std::expected<bool, Error> read_bool(std::string_view data, size_t &offset) {
  auto value = read_u8(data, offset);
  if (!value.has_value()) {
    return std::unexpected(value.error());
  }
  if (*value > 1) {
    return std::unexpected(malformed("boolean must be 0 or 1"));
  }
  return *value == 1;
}

void put_entry(std::string &out, const LogEntry &entry) {
  put_u64(out, entry.index);
  put_u64(out, entry.term);
  put_bytes(out, entry.data);
}

} // namespace

std::string encode_message(uint64_t group_id, const Message &message) {
  std::string out;
  put_u8(out, kMessageVersion);
  put_u64(out, group_id);

  std::visit(
      [&](const auto &concrete) {
        using T = std::decay_t<decltype(concrete)>;
        if constexpr (std::is_same_v<T, RequestVoteRequest>) {
          put_u8(out, static_cast<uint8_t>(MessageType::kRequestVoteRequest));
          put_u64(out, concrete.term);
          put_u64(out, concrete.candidate_id.value);
          put_u64(out, concrete.last_log_index);
          put_u64(out, concrete.last_log_term);
        } else if constexpr (std::is_same_v<T, RequestVoteResponse>) {
          put_u8(out, static_cast<uint8_t>(MessageType::kRequestVoteResponse));
          put_u64(out, concrete.term);
          put_u8(out, concrete.vote_granted ? 1 : 0);
        } else if constexpr (std::is_same_v<T, AppendEntriesRequest>) {
          put_u8(out,
                 static_cast<uint8_t>(MessageType::kAppendEntriesRequest));
          put_u64(out, concrete.term);
          put_u64(out, concrete.leader_id.value);
          put_u64(out, concrete.prev_log_index);
          put_u64(out, concrete.prev_log_term);
          put_u64(out, concrete.leader_commit);
          put_u64(out, concrete.round);
          put_u64(out, concrete.entries.size());
          for (const auto &entry : concrete.entries) {
            put_entry(out, entry.entry);
          }
        } else if constexpr (std::is_same_v<T, AppendEntriesResponse>) {
          put_u8(out,
                 static_cast<uint8_t>(MessageType::kAppendEntriesResponse));
          put_u64(out, concrete.term);
          put_u8(out, concrete.success ? 1 : 0);
          put_u64(out, concrete.match_index);
          put_u64(out, concrete.round);
          put_u64(out, concrete.hint_last_index);
        } else if constexpr (std::is_same_v<T, InstallSnapshotRequest>) {
          put_u8(out,
                 static_cast<uint8_t>(MessageType::kInstallSnapshotRequest));
          put_u64(out, concrete.term);
          put_u64(out, concrete.leader_id.value);
          put_u64(out, concrete.last_included_index);
          put_u64(out, concrete.last_included_term);
          put_u64(out, concrete.offset);
          put_u8(out, concrete.done ? 1 : 0);
          put_bytes(out, concrete.data);
        } else if constexpr (std::is_same_v<T, InstallSnapshotResponse>) {
          put_u8(out,
                 static_cast<uint8_t>(MessageType::kInstallSnapshotResponse));
          put_u64(out, concrete.term);
          put_u8(out, concrete.success ? 1 : 0);
        } else {
          static_assert(sizeof(T) == 0,
                        "unhandled raft message type in encode_message");
        }
      },
      message);
  return out;
}

std::expected<DecodedRaftMessage, Error>
decode_message(std::string_view payload) {
  size_t offset = 0;
  auto version = read_u8(payload, offset);
  if (!version.has_value()) {
    return std::unexpected(version.error());
  }
  if (*version != kMessageVersion) {
    return std::unexpected(
        malformed("unsupported version " + std::to_string(*version)));
  }
  auto group_id = read_u64(payload, offset);
  if (!group_id.has_value()) {
    return std::unexpected(group_id.error());
  }
  auto type = read_u8(payload, offset);
  if (!type.has_value()) {
    return std::unexpected(type.error());
  }

  const auto finish =
      [&](Message message) -> std::expected<DecodedRaftMessage, Error> {
    if (offset != payload.size()) {
      return std::unexpected(malformed("trailing bytes"));
    }
    return DecodedRaftMessage{*group_id, std::move(message)};
  };

  switch (static_cast<MessageType>(*type)) {
  case MessageType::kRequestVoteRequest: {
    RequestVoteRequest request;
    auto term = read_u64(payload, offset);
    auto candidate = read_u64(payload, offset);
    auto last_index = read_u64(payload, offset);
    auto last_term = read_u64(payload, offset);
    if (!term.has_value() || !candidate.has_value() ||
        !last_index.has_value() || !last_term.has_value()) {
      return std::unexpected(truncated("RequestVoteRequest"));
    }
    request.term = *term;
    request.candidate_id = NodeId{*candidate};
    request.last_log_index = *last_index;
    request.last_log_term = *last_term;
    return finish(request);
  }
  case MessageType::kRequestVoteResponse: {
    auto term = read_u64(payload, offset);
    auto granted = read_bool(payload, offset);
    if (!term.has_value() || !granted.has_value()) {
      return std::unexpected(truncated("RequestVoteResponse"));
    }
    return finish(
        RequestVoteResponse{*term, *granted});
  }
  case MessageType::kAppendEntriesRequest: {
    AppendEntriesRequest request;
    auto term = read_u64(payload, offset);
    auto leader = read_u64(payload, offset);
    auto prev_index = read_u64(payload, offset);
    auto prev_term = read_u64(payload, offset);
    auto leader_commit = read_u64(payload, offset);
    auto round = read_u64(payload, offset);
    auto count = read_u64(payload, offset);
    if (!term.has_value() || !leader.has_value() || !prev_index.has_value() ||
        !prev_term.has_value() || !leader_commit.has_value() ||
        !round.has_value() || !count.has_value()) {
      return std::unexpected(truncated("AppendEntriesRequest"));
    }
    if (*count > payload.size()) {
      return std::unexpected(malformed("entry count exceeds payload size"));
    }
    request.term = *term;
    request.leader_id = NodeId{*leader};
    request.prev_log_index = *prev_index;
    request.prev_log_term = *prev_term;
    request.leader_commit = *leader_commit;
    request.round = *round;
    request.entries.reserve(static_cast<size_t>(*count));
    for (uint64_t i = 0; i < *count; ++i) {
      LogEntry entry;
      auto index = read_u64(payload, offset);
      auto entry_term = read_u64(payload, offset);
      auto data = read_bytes(payload, offset);
      if (!index.has_value() || !entry_term.has_value() ||
          !data.has_value()) {
        return std::unexpected(truncated("AppendEntriesRequest entry"));
      }
      entry.index = *index;
      entry.term = *entry_term;
      entry.data = std::move(*data);
      request.entries.push_back(LogEntryMessage{std::move(entry)});
    }
    return finish(request);
  }
  case MessageType::kAppendEntriesResponse: {
    auto term = read_u64(payload, offset);
    auto success = read_bool(payload, offset);
    auto match_index = read_u64(payload, offset);
    auto round = read_u64(payload, offset);
    auto hint_last_index = read_u64(payload, offset);
    if (!term.has_value() || !success.has_value() ||
        !match_index.has_value() || !round.has_value() ||
        !hint_last_index.has_value()) {
      return std::unexpected(truncated("AppendEntriesResponse"));
    }
    return finish(AppendEntriesResponse{*term, *success, *match_index, *round,
                                        *hint_last_index});
  }
  case MessageType::kInstallSnapshotRequest: {
    InstallSnapshotRequest request;
    auto term = read_u64(payload, offset);
    auto leader = read_u64(payload, offset);
    auto last_included_index = read_u64(payload, offset);
    auto last_included_term = read_u64(payload, offset);
    auto chunk_offset = read_u64(payload, offset);
    auto done = read_bool(payload, offset);
    auto data = read_bytes(payload, offset);
    if (!term.has_value() || !leader.has_value() ||
        !last_included_index.has_value() || !last_included_term.has_value() ||
        !chunk_offset.has_value() || !done.has_value() ||
        !data.has_value()) {
      return std::unexpected(truncated("InstallSnapshotRequest"));
    }
    request.term = *term;
    request.leader_id = NodeId{*leader};
    request.last_included_index = *last_included_index;
    request.last_included_term = *last_included_term;
    request.offset = *chunk_offset;
    request.done = *done;
    request.data = std::move(*data);
    return finish(std::move(request));
  }
  case MessageType::kInstallSnapshotResponse: {
    auto term = read_u64(payload, offset);
    auto success = read_bool(payload, offset);
    if (!term.has_value() || !success.has_value()) {
      return std::unexpected(truncated("InstallSnapshotResponse"));
    }
    return finish(InstallSnapshotResponse{*term, *success});
  }
  }
  return std::unexpected(
      malformed("unknown message type " + std::to_string(*type)));
}

std::string encode_frame(uint64_t group_id, const Message &message) {
  return frame_payload(encode_message(group_id, message));
}

std::string frame_payload(std::string_view payload) {
  std::string frame;
  put_u32(frame, static_cast<uint32_t>(payload.size()));
  frame.append(payload);
  return frame;
}

std::expected<std::string, Error> unframe_payload(std::string_view frame) {
  size_t offset = 0;
  auto size = read_u32(frame, offset);
  if (!size.has_value()) {
    return std::unexpected(size.error());
  }
  if (frame.size() - offset != *size) {
    return std::unexpected(malformed(
        "frame size " + std::to_string(*size) + " does not match " +
        std::to_string(frame.size() - offset) + " bytes of payload"));
  }
  return std::string{frame.substr(offset)};
}

std::expected<DecodedRaftMessage, Error>
decode_frame(std::string_view frame) {
  auto payload = unframe_payload(frame);
  if (!payload.has_value()) {
    return std::unexpected(payload.error());
  }
  return decode_message(*payload);
}

std::expected<bool, Error> FrameDecoder::next(std::string *out) {
  if (out == nullptr) {
    return std::unexpected(
        Error{ErrorCode::InvalidArgument, "frame decoder needs an output"});
  }
  const std::string_view pending(buffer_.data() + consumed_,
                                 buffer_.size() - consumed_);
  if (pending.size() < sizeof(uint32_t)) {
    return false;
  }

  size_t offset = 0;
  auto size = read_u32(pending, offset);
  if (!size.has_value()) {
    return std::unexpected(size.error());
  }
  if (*size > kMaxFrameBytes) {
    return std::unexpected(malformed("frame size " + std::to_string(*size) +
                                     " exceeds the limit"));
  }
  if (pending.size() - offset < *size) {
    return false;
  }

  *out = std::string(pending.substr(offset, *size));
  consumed_ += offset + *size;
  if (consumed_ == buffer_.size()) {
    buffer_.clear();
    consumed_ = 0;
  }
  return true;
}

} // namespace raft
