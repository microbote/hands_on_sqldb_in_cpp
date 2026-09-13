// server/protocol.cpp
#include "protocol.h"

#include <cstring>

namespace server {
namespace {

void put_u16(std::string *out, uint16_t value) {
  out->push_back(static_cast<char>(value & 0xFF));
  out->push_back(static_cast<char>((value >> 8) & 0xFF));
}

void put_u32(std::string *out, uint32_t value) {
  for (int i = 0; i < 4; ++i) {
    out->push_back(static_cast<char>((value >> (8 * i)) & 0xFF));
  }
}

void put_u64(std::string *out, uint64_t value) {
  for (int i = 0; i < 8; ++i) {
    out->push_back(static_cast<char>((value >> (8 * i)) & 0xFF));
  }
}

void put_bytes(std::string *out, const std::string &bytes) {
  put_u32(out, static_cast<uint32_t>(bytes.size()));
  out->append(bytes);
}

bool get_u16(const std::string &in, size_t *pos, uint16_t *out) {
  if (*pos + 2 > in.size()) {
    return false;
  }
  *out = static_cast<uint16_t>(static_cast<unsigned char>(in[*pos])) |
         static_cast<uint16_t>(static_cast<unsigned char>(in[*pos + 1]) << 8);
  *pos += 2;
  return true;
}

bool get_u32(const std::string &in, size_t *pos, uint32_t *out) {
  if (*pos + 4 > in.size()) {
    return false;
  }
  uint32_t value = 0;
  for (int i = 0; i < 4; ++i) {
    value |= static_cast<uint32_t>(
                 static_cast<unsigned char>(in[*pos + static_cast<size_t>(i)]))
             << (8 * i);
  }
  *pos += 4;
  *out = value;
  return true;
}

bool get_u64(const std::string &in, size_t *pos, uint64_t *out) {
  if (*pos + 8 > in.size()) {
    return false;
  }
  uint64_t value = 0;
  for (int i = 0; i < 8; ++i) {
    value |= static_cast<uint64_t>(
                 static_cast<unsigned char>(in[*pos + static_cast<size_t>(i)]))
             << (8 * i);
  }
  *pos += 8;
  *out = value;
  return true;
}

bool get_bytes(const std::string &in, size_t *pos, std::string *out) {
  uint32_t length = 0;
  if (!get_u32(in, pos, &length)) {
    return false;
  }
  if (*pos + length > in.size()) {
    return false;
  }
  out->assign(in, *pos, length);
  *pos += length;
  return true;
}

} // namespace

std::string encode_frame(FrameType type, const std::string &payload) {
  std::string frame;
  frame.reserve(payload.size() + 5);
  frame.push_back(static_cast<char>(type));
  put_u32(&frame, static_cast<uint32_t>(payload.size()));
  frame.append(payload);
  return frame;
}

std::string encode_hello(uint16_t server_version, uint32_t capabilities) {
  std::string payload;
  put_u16(&payload, kProtocolVersion);
  put_u16(&payload, server_version);
  put_u32(&payload, capabilities);
  return encode_frame(FrameType::kHello, payload);
}

std::string encode_columns(const std::vector<std::string> &columns) {
  std::string payload;
  put_u16(&payload, static_cast<uint16_t>(columns.size()));
  for (const auto &name : columns) {
    put_bytes(&payload, name);
  }
  return encode_frame(FrameType::kColumns, payload);
}

std::string encode_row(const std::vector<ProtocolValue> &values) {
  std::string payload;
  put_u16(&payload, static_cast<uint16_t>(values.size()));
  for (const auto &value : values) {
    payload.push_back(value.is_null ? 1 : 0);
    put_bytes(&payload, value.text);
  }
  return encode_frame(FrameType::kRow, payload);
}

std::string encode_ok(uint64_t affected_rows) {
  std::string payload;
  put_u64(&payload, affected_rows);
  return encode_frame(FrameType::kOk, payload);
}

std::string encode_error(const ErrorFrame &error) {
  std::string payload;
  payload.push_back(static_cast<char>(error.code));
  put_bytes(&payload, error.message);
  put_bytes(&payload, error.sql);
  put_u32(&payload, error.begin_line);
  put_u32(&payload, error.begin_column);
  put_u32(&payload, error.end_line);
  put_u32(&payload, error.end_column);
  return encode_frame(FrameType::kError, payload);
}

std::string encode_query(const std::string &sql) {
  return encode_frame(FrameType::kQuery, sql);
}

std::string encode_simple(FrameType type) {
  return encode_frame(type, std::string());
}

bool try_decode_frame(const std::string &buffer, DecodedFrame *frame,
                      size_t *consumed, std::string *error) {
  *consumed = 0;
  if (buffer.size() < 5) {
    return false; // 头还没收全
  }
  const uint8_t type = static_cast<uint8_t>(buffer[0]);
  uint32_t length = 0;
  for (int i = 0; i < 4; ++i) {
    length |= static_cast<uint32_t>(static_cast<unsigned char>(
                  buffer[1 + static_cast<size_t>(i)]))
              << (8 * i);
  }
  if (length > kMaxFrameSize) {
    *error = "frame too large: " + std::to_string(length);
    return false;
  }
  if (buffer.size() < 5 + length) {
    return false; // 体还没收全
  }
  if (type < static_cast<uint8_t>(FrameType::kHello) ||
      type > static_cast<uint8_t>(FrameType::kBye)) {
    *error = "unknown frame type: " + std::to_string(type);
    return false;
  }
  frame->type = static_cast<FrameType>(type);
  frame->payload.assign(buffer, 5, length);
  *consumed = 5 + length;
  return true;
}

bool decode_query(const std::string &payload, std::string *sql) {
  *sql = payload;
  return true;
}

bool decode_hello(const std::string &payload, uint16_t *proto_version,
                  uint16_t *server_version, uint32_t *capabilities) {
  size_t pos = 0;
  return get_u16(payload, &pos, proto_version) &&
         get_u16(payload, &pos, server_version) &&
         get_u32(payload, &pos, capabilities);
}

bool decode_columns(const std::string &payload,
                    std::vector<std::string> *columns) {
  size_t pos = 0;
  uint16_t count = 0;
  if (!get_u16(payload, &pos, &count)) {
    return false;
  }
  columns->clear();
  for (uint16_t i = 0; i < count; ++i) {
    std::string name;
    if (!get_bytes(payload, &pos, &name)) {
      return false;
    }
    columns->push_back(std::move(name));
  }
  return true;
}

bool decode_row(const std::string &payload,
                std::vector<ProtocolValue> *values) {
  size_t pos = 0;
  uint16_t count = 0;
  if (!get_u16(payload, &pos, &count)) {
    return false;
  }
  values->clear();
  for (uint16_t i = 0; i < count; ++i) {
    if (pos + 1 > payload.size()) {
      return false;
    }
    ProtocolValue value;
    value.is_null = payload[pos] != 0;
    ++pos;
    if (!get_bytes(payload, &pos, &value.text)) {
      return false;
    }
    values->push_back(std::move(value));
  }
  return true;
}

bool decode_ok(const std::string &payload, uint64_t *affected_rows) {
  size_t pos = 0;
  return get_u64(payload, &pos, affected_rows);
}

bool decode_error(const std::string &payload, ErrorFrame *error) {
  size_t pos = 0;
  if (pos + 1 > payload.size()) {
    return false;
  }
  error->code = static_cast<uint8_t>(payload[pos]);
  ++pos;
  return get_bytes(payload, &pos, &error->message) &&
         get_bytes(payload, &pos, &error->sql) &&
         get_u32(payload, &pos, &error->begin_line) &&
         get_u32(payload, &pos, &error->begin_column) &&
         get_u32(payload, &pos, &error->end_line) &&
         get_u32(payload, &pos, &error->end_column);
}

} // namespace server
