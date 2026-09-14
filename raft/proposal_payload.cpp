#include "raft/proposal_payload.h"

#include <cstddef>

namespace raft {
namespace {

constexpr std::string_view kMagic = "SQRA";
constexpr uint32_t kPayloadVersion = 1;
constexpr size_t kFixedHeaderSize = kMagic.size() + sizeof(uint32_t) +
                                    sizeof(uint64_t) * 3;

void append_u32(std::string &out, uint32_t value) {
  out.push_back(static_cast<char>((value >> 24) & 0xff));
  out.push_back(static_cast<char>((value >> 16) & 0xff));
  out.push_back(static_cast<char>((value >> 8) & 0xff));
  out.push_back(static_cast<char>(value & 0xff));
}

void append_u64(std::string &out, uint64_t value) {
  for (int shift = 56; shift >= 0; shift -= 8) {
    out.push_back(static_cast<char>((value >> shift) & 0xff));
  }
}

void append_bytes(std::string &out, std::string_view value) {
  append_u64(out, value.size());
  out.append(value.data(), value.size());
}

class Decoder {
public:
  explicit Decoder(std::string_view data) : data_(data) {}

  std::expected<uint32_t, Error> u32() {
    if (offset_ + sizeof(uint32_t) > data_.size()) {
      return truncated();
    }
    uint32_t value = 0;
    for (size_t i = 0; i < sizeof(uint32_t); ++i) {
      value = (value << 8) |
              static_cast<uint32_t>(
                  static_cast<unsigned char>(data_[offset_ + i]));
    }
    offset_ += sizeof(uint32_t);
    return value;
  }

  std::expected<uint64_t, Error> u64() {
    if (offset_ + sizeof(uint64_t) > data_.size()) {
      return truncated();
    }
    uint64_t value = 0;
    for (size_t i = 0; i < sizeof(uint64_t); ++i) {
      value = (value << 8) |
              static_cast<uint64_t>(
                  static_cast<unsigned char>(data_[offset_ + i]));
    }
    offset_ += sizeof(uint64_t);
    return value;
  }

  std::expected<uint8_t, Error> u8() {
    if (offset_ >= data_.size()) {
      return truncated();
    }
    const auto value = static_cast<uint8_t>(data_[offset_++]);
    return value;
  }

  std::expected<std::string, Error> bytes() {
    auto size = u64();
    if (!size.has_value()) {
      return std::unexpected(size.error());
    }
    if (*size > data_.size() - offset_) {
      return truncated();
    }
    std::string value{data_.substr(offset_, *size)};
    offset_ += *size;
    return value;
  }

  bool at_end() const { return offset_ == data_.size(); }

private:
  static std::unexpected<Error> truncated() {
    return std::unexpected(
        Error{ErrorCode::InvalidArgument, "truncated proposal payload"});
  }

  std::string_view data_;
  size_t offset_ = 0;
};

} // namespace

std::string encode_proposal_payload(const ProposalPayload &payload) {
  std::string out;
  out.reserve(kFixedHeaderSize);
  out.append(kMagic);
  append_u32(out, kPayloadVersion);
  append_u64(out, payload.client_id);
  append_u64(out, payload.request_id);
  append_u64(out, payload.batch.ops().size());

  for (const auto &op : payload.batch.ops()) {
    switch (op.type) {
    case kv::WriteBatch::OpType::kPut:
      out.push_back('\x01');
      break;
    case kv::WriteBatch::OpType::kRemove:
      out.push_back('\x02');
      break;
    case kv::WriteBatch::OpType::kRemoveRange:
      out.push_back('\x03');
      break;
    }

    append_bytes(out, op.data.key);
    if (op.data.value.has_value()) {
      out.push_back('\x01');
      append_bytes(out, *op.data.value);
    } else {
      out.push_back('\x00');
    }
    append_bytes(out, op.range_end);
  }

  return out;
}

std::expected<ProposalPayload, Error>
decode_proposal_payload(std::string_view data) {
  if (data.size() < kFixedHeaderSize ||
      data.substr(0, kMagic.size()) != kMagic) {
    return std::unexpected(
        Error{ErrorCode::InvalidArgument, "invalid proposal payload magic"});
  }

  Decoder decoder{data.substr(kMagic.size())};

  const auto version = decoder.u32();
  if (!version.has_value()) {
    return std::unexpected(version.error());
  }
  if (*version != kPayloadVersion) {
    return std::unexpected(
        Error{ErrorCode::InvalidArgument, "unsupported proposal payload version"});
  }

  ProposalPayload payload;
  auto client_id = decoder.u64();
  if (!client_id.has_value()) {
    return std::unexpected(client_id.error());
  }
  payload.client_id = *client_id;

  auto request_id = decoder.u64();
  if (!request_id.has_value()) {
    return std::unexpected(request_id.error());
  }
  payload.request_id = *request_id;

  auto op_count = decoder.u64();
  if (!op_count.has_value()) {
    return std::unexpected(op_count.error());
  }
  if (*op_count > static_cast<uint64_t>(data.size())) {
    return std::unexpected(Error{
        ErrorCode::InvalidArgument, "invalid proposal payload op count"});
  }

  for (uint64_t i = 0; i < *op_count; ++i) {
    auto type = decoder.u8();
    if (!type.has_value()) {
      return std::unexpected(type.error());
    }

    auto key = decoder.bytes();
    if (!key.has_value()) {
      return std::unexpected(key.error());
    }
    auto has_value = decoder.u8();
    if (!has_value.has_value()) {
      return std::unexpected(has_value.error());
    }
    if (*has_value != 0 && *has_value != 1) {
      return std::unexpected(
          Error{ErrorCode::InvalidArgument, "invalid value presence flag"});
    }

    std::optional<kv::ByteValue> value;
    if (*has_value == 1) {
      auto decoded_value = decoder.bytes();
      if (!decoded_value.has_value()) {
        return std::unexpected(decoded_value.error());
      }
      value = std::move(*decoded_value);
    }

    auto range_end = decoder.bytes();
    if (!range_end.has_value()) {
      return std::unexpected(range_end.error());
    }

    switch (*type) {
    case 1:
      if (!value.has_value()) {
        return std::unexpected(Error{
            ErrorCode::InvalidArgument, "put operation has no value"});
      }
      payload.batch.put(*key, *value);
      break;
    case 2:
      payload.batch.remove(*key);
      break;
    case 3:
      if (*range_end <= *key) {
        return std::unexpected(Error{
            ErrorCode::InvalidArgument, "invalid remove_range bounds"});
      }
      payload.batch.remove_range(*key, *range_end);
      break;
    default:
      return std::unexpected(
          Error{ErrorCode::InvalidArgument, "unknown proposal operation type"});
    }
  }

  if (!decoder.at_end()) {
    return std::unexpected(
        Error{ErrorCode::InvalidArgument, "trailing bytes in proposal payload"});
  }
  return payload;
}

} // namespace raft
