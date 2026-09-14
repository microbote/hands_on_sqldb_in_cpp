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
  auto decoded_vote = raft::decode_frame(raft::encode_frame(vote));
  CHECK_TRUE(decoded_vote.has_value());
  if (decoded_vote.has_value()) {
    const auto &out = std::get<raft::RequestVoteRequest>(*decoded_vote);
    CHECK_EQ(out.term, uint64_t{7});
    CHECK_EQ(out.candidate_id.value, uint64_t{3});
    CHECK_EQ(out.last_log_index, uint64_t{42});
    CHECK_EQ(out.last_log_term, uint64_t{5});
  }

  const raft::RequestVoteResponse vote_response{9, false};
  auto decoded_vote_response =
      raft::decode_frame(raft::encode_frame(vote_response));
  CHECK_TRUE(decoded_vote_response.has_value());
  if (decoded_vote_response.has_value()) {
    const auto &out = std::get<raft::RequestVoteResponse>(*decoded_vote_response);
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

  auto decoded_append = raft::decode_frame(raft::encode_frame(append));
  CHECK_TRUE(decoded_append.has_value());
  if (decoded_append.has_value()) {
    const auto &out = std::get<raft::AppendEntriesRequest>(*decoded_append);
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
      raft::decode_frame(raft::encode_frame(append_response));
  CHECK_TRUE(decoded_append_response.has_value());
  if (decoded_append_response.has_value()) {
    const auto &out =
        std::get<raft::AppendEntriesResponse>(*decoded_append_response);
    CHECK_EQ(out.term, uint64_t{12});
    CHECK_TRUE(out.success);
    CHECK_EQ(out.match_index, uint64_t{6});
    CHECK_EQ(out.round, uint64_t{8});
  }
}

TEST(MessageCodec, RejectsMalformedPayloads) {
  // Empty payload / missing header.
  CHECK_FALSE(raft::decode_message("").has_value());

  // Unknown version.
  std::string payload = raft::encode_message(raft::RequestVoteResponse{1, true});
  payload[0] = static_cast<char>(raft::kMessageVersion + 1);
  CHECK_FALSE(raft::decode_message(payload).has_value());

  // Unknown message type.
  CHECK_FALSE(raft::decode_message(std::string{"\x01\x63", 2}).has_value());

  // Truncated body.
  const std::string full =
      raft::encode_message(raft::AppendEntriesResponse{1, true, 2, 3});
  for (size_t cut = 1; cut < full.size(); ++cut) {
    CHECK_FALSE(raft::decode_message(full.substr(0, cut)).has_value());
  }

  // Trailing bytes (std::string + "\0" would stop at the NUL: append a byte).
  CHECK_FALSE(raft::decode_message(full + std::string(1, '\0')).has_value());

  // A malformed boolean (must be 0 or 1).
  std::string bad_bool = full;
  bad_bool[10] = static_cast<char>(2); // version + type + term(8) = offset 10
  CHECK_FALSE(raft::decode_message(bad_bool).has_value());

  // Frame size must match the payload exactly.
  const std::string frame = raft::encode_frame(raft::AppendEntriesResponse{1, true, 2, 3});
  CHECK_FALSE(raft::decode_frame(frame.substr(0, frame.size() - 1)).has_value());
  CHECK_FALSE(raft::decode_frame(frame + std::string(1, '\0')).has_value());
}

TEST(MessageCodec, FrameDecoderHandlesFragmentedAndBatchedInput) {
  const std::string first = raft::encode_frame(raft::RequestVoteRequest{1, NodeId{2}, 0, 0});
  const std::string second =
      raft::encode_frame(raft::AppendEntriesResponse{3, true, 1, 4});

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
    CHECK_EQ(std::get<raft::RequestVoteRequest>(*decoded).candidate_id.value,
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
