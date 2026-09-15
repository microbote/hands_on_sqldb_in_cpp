#include <cstdint>
#include <string>
#include <variant>
#include <vector>

#include "raft/message_codec.h"
#include "test_framework.h"

namespace {

using raft::NodeId;

TEST(MessageCodec, RoundTripsEveryMessageType) {
  const raft::RequestVoteRequest vote{7, NodeId{3}, 42, 5};
  auto decoded_vote = raft::decode_frame(raft::encode_frame(0, vote));
  CHECK_TRUE(decoded_vote.has_value());
  if (decoded_vote.has_value()) {
    const auto &out = std::get<raft::RequestVoteRequest>(decoded_vote->message);
    CHECK_EQ(out.term, uint64_t{7});
    CHECK_EQ(out.candidate_id.value, uint64_t{3});
    CHECK_EQ(out.last_log_index, uint64_t{42});
    CHECK_EQ(out.last_log_term, uint64_t{5});
  }

  const raft::RequestVoteResponse vote_response{9, false};
  auto decoded_vote_response =
      raft::decode_frame(raft::encode_frame(0, vote_response));
  CHECK_TRUE(decoded_vote_response.has_value());
  if (decoded_vote_response.has_value()) {
    const auto &out = std::get<raft::RequestVoteResponse>(decoded_vote_response->message);
    CHECK_EQ(out.term, uint64_t{9});
    CHECK_FALSE(out.vote_granted);
  }

  raft::AppendEntriesRequest append;
  append.term = 11;
  append.leader_id = NodeId{2};
  append.prev_log_index = 3;
  append.prev_log_term = 10;
  append.leader_commit = 2;
  append.round = 8;
  // Binary-safe payload: entry data is a serialized WriteBatch in production.
  const std::string binary_data{"SQRA\x00\x01\xff", 8};
  append.entries.push_back(
      raft::LogEntryMessage{raft::LogEntry{4, 11, binary_data}});
  append.entries.push_back(
      raft::LogEntryMessage{raft::LogEntry{5, 11, ""}});

  auto decoded_append = raft::decode_frame(raft::encode_frame(0, append));
  CHECK_TRUE(decoded_append.has_value());
  if (decoded_append.has_value()) {
    const auto &out = std::get<raft::AppendEntriesRequest>(decoded_append->message);
    CHECK_EQ(out.term, uint64_t{11});
    CHECK_EQ(out.leader_id.value, uint64_t{2});
    CHECK_EQ(out.prev_log_index, uint64_t{3});
    CHECK_EQ(out.prev_log_term, uint64_t{10});
    CHECK_EQ(out.leader_commit, uint64_t{2});
    CHECK_EQ(out.round, uint64_t{8});
    CHECK_EQ(out.entries.size(), size_t{2});
    if (out.entries.size() == 2) {
      CHECK_EQ(out.entries[0].entry.index, uint64_t{4});
      CHECK_EQ(out.entries[0].entry.term, uint64_t{11});
      CHECK_EQ(out.entries[0].entry.data, binary_data);
      CHECK_EQ(out.entries[1].entry.index, uint64_t{5});
      CHECK_TRUE(out.entries[1].entry.data.empty());
    }
  }

  const raft::AppendEntriesResponse append_response{12, true, 6, 8};
  auto decoded_append_response =
      raft::decode_frame(raft::encode_frame(0, append_response));
  CHECK_TRUE(decoded_append_response.has_value());
  if (decoded_append_response.has_value()) {
    const auto &out =
        std::get<raft::AppendEntriesResponse>(decoded_append_response->message);
    CHECK_EQ(out.term, uint64_t{12});
    CHECK_TRUE(out.success);
    CHECK_EQ(out.match_index, uint64_t{6});
    CHECK_EQ(out.round, uint64_t{8});
  }

  raft::InstallSnapshotRequest snapshot_request;
  snapshot_request.term = 13;
  snapshot_request.leader_id = NodeId{1};
  snapshot_request.last_included_index = 40;
  snapshot_request.last_included_term = 11;
  snapshot_request.offset = 5;
  snapshot_request.done = false;
  const std::string snapshot_data{"SQSN\x00\x01\xff", 8};
  snapshot_request.data = snapshot_data;
  auto decoded_snapshot_request =
      raft::decode_frame(raft::encode_frame(0, snapshot_request));
  CHECK_TRUE(decoded_snapshot_request.has_value());
  if (decoded_snapshot_request.has_value()) {
    const auto &out =
        std::get<raft::InstallSnapshotRequest>(decoded_snapshot_request->message);
    CHECK_EQ(out.term, uint64_t{13});
    CHECK_EQ(out.leader_id.value, uint64_t{1});
    CHECK_EQ(out.last_included_index, uint64_t{40});
    CHECK_EQ(out.last_included_term, uint64_t{11});
    CHECK_EQ(out.offset, uint64_t{5});
    CHECK_FALSE(out.done);
    CHECK_EQ(out.data, snapshot_data);
  }

  const raft::InstallSnapshotResponse snapshot_response{14, true};
  auto decoded_snapshot_response =
      raft::decode_frame(raft::encode_frame(0, snapshot_response));
  CHECK_TRUE(decoded_snapshot_response.has_value());
  if (decoded_snapshot_response.has_value()) {
    const auto &out =
        std::get<raft::InstallSnapshotResponse>(decoded_snapshot_response->message);
    CHECK_EQ(out.term, uint64_t{14});
    CHECK_TRUE(out.success);
  }
}

TEST(MessageCodec, RoundTripsGroupId) {
  const raft::AppendEntriesRequest message{7, NodeId{3}, 2, 1, {}, 5, 4};
  auto decoded = raft::decode_frame(raft::encode_frame(42, message));
  CHECK_TRUE(decoded.has_value());
  if (decoded.has_value()) {
    CHECK_EQ(decoded->group_id, uint64_t{42});
    CHECK_TRUE(std::holds_alternative<raft::AppendEntriesRequest>(
        decoded->message));
    const auto &out = std::get<raft::AppendEntriesRequest>(decoded->message);
    CHECK_EQ(out.term, uint64_t{7});
    CHECK_EQ(out.round, uint64_t{4});
  }
}

TEST(MessageCodec, RejectsMalformedPayloads) {
  // Empty payload / missing header.
  CHECK_FALSE(raft::decode_message("").has_value());

  // Unknown version.
  std::string payload = raft::encode_message(0, raft::RequestVoteResponse{1, true});
  payload[0] = static_cast<char>(raft::kMessageVersion + 1);
  CHECK_FALSE(raft::decode_message(payload).has_value());

  // Unknown message type.
  CHECK_FALSE(raft::decode_message(std::string{"\x01\x63", 2}).has_value());

  // Truncated body.
  const std::string full =
      raft::encode_message(0, raft::AppendEntriesResponse{1, true, 2, 3});
  for (size_t cut = 1; cut < full.size(); ++cut) {
    CHECK_FALSE(raft::decode_message(full.substr(0, cut)).has_value());
  }

  // Trailing bytes (std::string + "\0" would stop at the NUL: append a byte).
  CHECK_FALSE(raft::decode_message(full + std::string(1, '\0')).has_value());

  // A malformed boolean (must be 0 or 1).
  std::string bad_bool = full;
  // Layout: version(0) + group_id(1..8) + type(9) + term(10..17) + success(18).
  bad_bool[18] = static_cast<char>(2);
  CHECK_FALSE(raft::decode_message(bad_bool).has_value());

  // Frame size must match the payload exactly.
  const std::string frame = raft::encode_frame(0, raft::AppendEntriesResponse{1, true, 2, 3});
  CHECK_FALSE(raft::decode_frame(frame.substr(0, frame.size() - 1)).has_value());
  CHECK_FALSE(raft::decode_frame(frame + std::string(1, '\0')).has_value());
}

TEST(MessageCodec, FrameDecoderHandlesFragmentedAndBatchedInput) {
  const std::string first = raft::encode_frame(0, raft::RequestVoteRequest{1, NodeId{2}, 0, 0});
  const std::string second =
      raft::encode_frame(0, raft::AppendEntriesResponse{3, true, 1, 4});

  raft::FrameDecoder decoder;
  std::string frame;
  CHECK_FALSE(decoder.next(&frame).value());

  // Header only, then a partial payload: no frame yet.
  decoder.append(std::string_view(first).substr(0, 4));
  CHECK_FALSE(decoder.next(&frame).value());
  decoder.append(std::string_view(first).substr(4, first.size() - 8));
  CHECK_FALSE(decoder.next(&frame).value());

  // Final bytes complete the first frame, then a second one arrives in the
  // same chunk.
  decoder.append(std::string_view(first).substr(first.size() - 4));
  auto more = decoder.next(&frame);
  CHECK_TRUE(more.has_value());
  CHECK_TRUE(more.value());
  auto decoded = raft::decode_message(frame);
  CHECK_TRUE(decoded.has_value());
  if (decoded.has_value()) {
    CHECK_EQ(std::get<raft::RequestVoteRequest>(decoded->message).candidate_id.value,
             uint64_t{2});
  }

  decoder.append(first + second);
  auto first_again = decoder.next(&frame);
  CHECK_TRUE(first_again.has_value());
  CHECK_TRUE(first_again.value());
  auto second_frame = decoder.next(&frame);
  CHECK_TRUE(second_frame.has_value());
  CHECK_TRUE(second_frame.value());
  CHECK_EQ(decoder.buffered_bytes(), size_t{0});
  CHECK_FALSE(decoder.next(&frame).value());
}

TEST(MessageCodec, FrameDecoderRejectsImpossibleSizePrefix) {
  raft::FrameDecoder decoder;
  // u32 = kMaxFrameBytes + 1
  const uint32_t too_big = raft::kMaxFrameBytes + 1;
  std::string prefix;
  for (int shift = 24; shift >= 0; shift -= 8) {
    prefix.push_back(
        static_cast<char>((too_big >> shift) & 0xff));
  }
  decoder.append(prefix);
  std::string frame;
  auto result = decoder.next(&frame);
  CHECK_FALSE(result.has_value());
}

} // namespace
