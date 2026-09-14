// common/proto/protocol.h
//
// sqldb 自有协议（M1：文本 + NULL 标志 + 一帧一行）。
//
// 放在 common/ 下是因为**两端共用**：server 收帧/回帧、client 发帧/解帧。
// （原来在 server/protocol.h，客户端为了编解码也得去 include "server/..."，
// 层次不对，2026-09-14 搬到 common/proto/，命名空间 server -> common::proto。）
//
//   frame = [u8 type][u32 payload_len][payload]      （net 序，小端）
//
// 帧类型（payload 布局）：
//   1 HELLO   server->client : u16 proto_version, u16 server_version,
//                              u32 capabilities
//   2 QUERY   client->server : UTF-8 SQL（一条语句，可带结尾 ';'）
//   3 COLUMNS server->client : u16 count, { u32 len, bytes name }*
//   4 ROW     server->client : u16 count, { u8 null_flag, u32 len, bytes text
//   }* 5 OK      server->client : u64 affected_rows,
//                              u8 flags(bit0=in_transaction, bit1=is_write),
//                              bytes current_database
//   6 ERROR   server->client : u8 code, bytes message, bytes sql,
//                              u32 begin_line, u32 begin_col,
//                              u32 end_line, u32 end_col
//   7 PING / 8 BYE（无 payload）
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

#include "sql_types/schema.h"

namespace common::proto {

enum class FrameType : uint8_t {
  kHello = 1,
  kQuery = 2,
  kColumns = 3,
  kRow = 4,
  kOk = 5,
  kError = 6,
  kPing = 7,
  kBye = 8,
  kMeta = 9,      // client->server：要元信息（\l / \dt / \d）
  kMetaReply = 10 // server->client：元信息载荷（按 kind 解码）
};

// 元信息请求的种类（kMeta 的 payload：u8 kind, bytes arg1, bytes arg2）
enum class MetaKind : uint8_t {
  kDatabases = 1, // arg1/arg2 空
  kTables = 2,    // arg1 = 库名（空 = 当前库）
  kSchema = 3,    // arg1 = 库名（空 = 当前库），arg2 = 表名
};

struct MetaDatabase {
  std::string name;
  int64_t created_at = 0;
  uint32_t table_count = 0;
  bool is_current = false;
};

struct MetaTable {
  std::string name;
  uint32_t column_count = 0;
  std::string primary_key;
  int64_t created_at = 0;
  int64_t last_write_at = 0;
  uint64_t row_count = 0;
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
std::string encode_ok(uint64_t affected_rows, bool in_transaction,
                      bool is_write, const std::string &current_database);
std::string encode_error(const ErrorFrame &error);
std::string encode_query(const std::string &sql);
std::string encode_simple(FrameType type);
std::string encode_frame(FrameType type, const std::string &payload);

// ---- 元信息（META）----
std::string encode_meta(MetaKind kind, const std::string &arg1,
                        const std::string &arg2 = std::string());
bool decode_meta(const std::string &payload, uint8_t *kind, std::string *arg1,
                 std::string *arg2);
std::string encode_meta_reply(const std::string &payload);
bool decode_meta_reply(const std::string &payload, std::string *out);
// kind = kDatabases / kTables 的载荷
std::string encode_meta_databases(const std::vector<MetaDatabase> &databases);
bool decode_meta_databases(const std::string &payload,
                           std::vector<MetaDatabase> *databases);
std::string encode_meta_tables(const std::vector<MetaTable> &tables);
bool decode_meta_tables(const std::string &payload,
                        std::vector<MetaTable> *tables);
// kind = kSchema 的载荷：直接复用 TableSchema 的序列化（sql_types v1）
std::string encode_meta_schema(const sql::TableSchema &schema);
bool decode_meta_schema(const std::string &payload, sql::TableSchema *schema);

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
bool decode_ok(const std::string &payload, uint64_t *affected_rows,
               uint8_t *flags, std::string *current_database);
bool decode_error(const std::string &payload, ErrorFrame *error);

} // namespace common::proto
