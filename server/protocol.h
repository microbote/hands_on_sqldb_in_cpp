// server/protocol.h
//
// sqldb 自有协议（M1：文本 + NULL 标志 + 一帧一行）。
//
//   frame = [u8 type][u32 payload_len][payload]      （net 序，小端）
//
//   1 HELLO   server→client : u16 proto_version, u16 server_version, u32
//   capabilities 2 QUERY   client→server : UTF-8 SQL（一条语句，可以带结尾
//   ';'） 3 COLUMNS server→client : u16 count, { u32 len, bytes name }* 4 ROW
//   server→client : u16 count, { u8 null_flag, u32 len, bytes text }* 5 OK
//   server→client : u64 affected_rows 6 ERROR   server→client : u8 code, u32
//   msg_len/bytes, u32 sql_len/bytes,
//                             u32 begin_line, u32 begin_col, u32 end_line, u32
//                             end_col
//   7 PING / 8 BYE
//
// 为什么 NULL 要带标志位：`Value::to_string()` 对 NULL 返回字面量 "NULL"，
// 与字符串 'NULL' 无法区分 —— 不给标志位就是数据说谎。
// 为什么错误带 span 而不是高亮文本：服务端不做 lexer 高亮（`lex_collect_tokens`
// 非重入），把 (code, message, sql, span) 给客户端，客户端用
// `stmt::highlight_span` 渲染 caret（CLI 已有这条路径）。
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace server {

enum class FrameType : uint8_t {
  kHello = 1,
  kQuery = 2,
  kColumns = 3,
  kRow = 4,
  kOk = 5,
  kError = 6,
  kPing = 7,
  kBye = 8,
};

constexpr uint16_t kProtocolVersion = 1;
// 单帧上限（防止对端用超大长度头把服务端撑爆）
constexpr uint32_t kMaxFrameSize = 16u * 1024 * 1024;

// 一个结果值：NULL 与空串是两回事
struct ProtocolValue {
  bool is_null = false;
  std::string text;
};

struct ErrorFrame {
  uint8_t code = 0; // session::SessionErrorCode
  std::string message;
  std::string sql; // 原文（客户端高亮用）
  uint32_t begin_line = 0;
  uint32_t begin_column = 0;
  uint32_t end_line = 0;
  uint32_t end_column = 0;
};

// ---- 编码（返回完整的帧字节，含帧头）----
std::string encode_hello(uint16_t server_version, uint32_t capabilities);
std::string encode_columns(const std::vector<std::string> &columns);
std::string encode_row(const std::vector<ProtocolValue> &values);
std::string encode_ok(uint64_t affected_rows);
std::string encode_error(const ErrorFrame &error);
std::string encode_query(const std::string &sql);
std::string encode_simple(FrameType type);
std::string encode_frame(FrameType type, const std::string &payload);

// ---- 解码 ----
struct DecodedFrame {
  FrameType type = FrameType::kPing;
  std::string payload;
};
// 从 buf（可能含多帧/半帧）里取出一帧：成功返回 true 并推进 consumed；
// 帧还没收全返回 false 且 consumed = 0；**协议错误**返回 false 且 error 有值
bool try_decode_frame(const std::string &buffer, DecodedFrame *frame,
                      size_t *consumed, std::string *error);

bool decode_query(const std::string &payload, std::string *sql);
bool decode_hello(const std::string &payload, uint16_t *proto_version,
                  uint16_t *server_version, uint32_t *capabilities);
bool decode_columns(const std::string &payload,
                    std::vector<std::string> *columns);
bool decode_row(const std::string &payload, std::vector<ProtocolValue> *values);
bool decode_ok(const std::string &payload, uint64_t *affected_rows);
bool decode_error(const std::string &payload, ErrorFrame *error);

} // namespace server
