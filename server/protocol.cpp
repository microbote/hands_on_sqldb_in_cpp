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

std::string encode_ok(uint64_t affected_rows, bool in_transaction,
                      bool is_write, const std::string &current_database) {
  std::string payload;
  put_u64(&payload, affected_rows);
  uint8_t flags = 0;
  if (in_transaction) {
    flags |= 0x1;
  }
  if (is_write) {
    flags |= 0x2;
  }
  payload.push_back(static_cast<char>(flags));
  put_bytes(&payload, current_database);
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

// ---- 元信息 ----
std::string encode_meta(MetaKind kind, const std::string &arg1,
                        const std::string &arg2) {
  std::string payload;
  payload.push_back(static_cast<char>(kind));
  put_bytes(&payload, arg1);
  put_bytes(&payload, arg2);
  return encode_frame(FrameType::kMeta, payload);
}

bool decode_meta(const std::string &payload, uint8_t *kind, std::string *arg1,
                 std::string *arg2) {
  size_t pos = 0;
  if (pos + 1 > payload.size()) {
    return false;
  }
  *kind = static_cast<uint8_t>(payload[pos]);
  ++pos;
  return get_bytes(payload, &pos, arg1) && get_bytes(payload, &pos, arg2);
}

std::string encode_meta_reply(const std::string &payload) {
  return encode_frame(FrameType::kMetaReply, payload);
}

bool decode_meta_reply(const std::string &payload, std::string *out) {
  *out = payload;
  return true;
}

std::string encode_meta_databases(const std::vector<MetaDatabase> &databases) {
  std::string payload;
  put_u16(&payload, static_cast<uint16_t>(databases.size()));
  for (const auto &db : databases) {
    put_bytes(&payload, db.name);
    put_u64(&payload, static_cast<uint64_t>(db.created_at));
    put_u32(&payload, db.table_count);
    payload.push_back(db.is_current ? 1 : 0);
  }
  return payload;
}

bool decode_meta_databases(const std::string &payload,
                           std::vector<MetaDatabase> *databases) {
  size_t pos = 0;
  uint16_t count = 0;
  if (!get_u16(payload, &pos, &count)) {
    return false;
  }
  databases->clear();
  for (uint16_t i = 0; i < count; ++i) {
    MetaDatabase db;
    uint64_t created = 0;
    if (!get_bytes(payload, &pos, &db.name) ||
        !get_u64(payload, &pos, &created) ||
        !get_u32(payload, &pos, &db.table_count) || pos + 1 > payload.size()) {
      return false;
    }
    db.created_at = static_cast<int64_t>(created);
    db.is_current = payload[pos] != 0;
    ++pos;
    databases->push_back(std::move(db));
  }
  return true;
}

std::string encode_meta_tables(const std::vector<MetaTable> &tables) {
  std::string payload;
  put_u16(&payload, static_cast<uint16_t>(tables.size()));
  for (const auto &table : tables) {
    put_bytes(&payload, table.name);
    put_u32(&payload, table.column_count);
    put_bytes(&payload, table.primary_key);
    put_u64(&payload, static_cast<uint64_t>(table.created_at));
    put_u64(&payload, static_cast<uint64_t>(table.last_write_at));
    put_u64(&payload, table.row_count);
  }
  return payload;
}

bool decode_meta_tables(const std::string &payload,
                        std::vector<MetaTable> *tables) {
  size_t pos = 0;
  uint16_t count = 0;
  if (!get_u16(payload, &pos, &count)) {
    return false;
  }
  tables->clear();
  for (uint16_t i = 0; i < count; ++i) {
    MetaTable table;
    uint64_t created = 0;
    uint64_t last_write = 0;
    if (!get_bytes(payload, &pos, &table.name) ||
        !get_u32(payload, &pos, &table.column_count) ||
        !get_bytes(payload, &pos, &table.primary_key) ||
        !get_u64(payload, &pos, &created) ||
        !get_u64(payload, &pos, &last_write) ||
        !get_u64(payload, &pos, &table.row_count)) {
      return false;
    }
    table.created_at = static_cast<int64_t>(created);
    table.last_write_at = static_cast<int64_t>(last_write);
    tables->push_back(std::move(table));
  }
  return true;
}

std::string encode_meta_schema(const sql::TableSchema &schema) {
  return schema.serialize();
}

bool decode_meta_schema(const std::string &payload, sql::TableSchema *schema) {
  auto decoded = sql::TableSchema::deserialize(payload);
  if (!decoded.has_value()) {
    return false;
  }
  *schema = std::move(*decoded);
  return true;
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
      type > static_cast<uint8_t>(FrameType::kMetaReply)) {
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

bool decode_ok(const std::string &payload, uint64_t *affected_rows,
               uint8_t *flags, std::string *current_database) {
  size_t pos = 0;
  if (!get_u64(payload, &pos, affected_rows)) {
    return false;
  }
  if (pos + 1 > payload.size()) {
    return false;
  }
  *flags = static_cast<uint8_t>(payload[pos]);
  ++pos;
  return get_bytes(payload, &pos, current_database);
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
