// tests/test_server/test_protocol.cpp
//
// 协议编解码：往返、NULL 与空串必须可区分、半帧/粘包、超长帧与未知类型被拒。
#include "test_framework.h"

#include "common/proto/protocol.h"

#include <string>
#include <vector>

TEST(Protocol, HelloRoundTrip) {
  const std::string frame = common::proto::encode_hello(7, 0x3);
  common::proto::DecodedFrame decoded;
  size_t consumed = 0;
  std::string error;
  CHECK(common::proto::try_decode_frame(frame, &decoded, &consumed, &error));
  CHECK_EQ(consumed, frame.size());
  CHECK(decoded.type == common::proto::FrameType::kHello);

  uint16_t proto = 0;
  uint16_t version = 0;
  uint32_t caps = 0;
  CHECK(common::proto::decode_hello(decoded.payload, &proto, &version, &caps));
  CHECK_EQ(proto, common::proto::kProtocolVersion);
  CHECK_EQ(version, uint16_t{7});
  CHECK_EQ(caps, uint32_t{3});
}

TEST(Protocol, ClientOptionsRoundTrip) {
  // capabilities + 空选项表。
  const std::string frame = common::proto::encode_client_options(
      common::proto::kCapabilityCrossGroupReadLoose);
  common::proto::DecodedFrame decoded;
  size_t consumed = 0;
  std::string error;
  CHECK(common::proto::try_decode_frame(frame, &decoded, &consumed, &error));
  CHECK_EQ(consumed, frame.size());
  CHECK(decoded.type == common::proto::FrameType::kClientOptions);

  uint32_t capabilities = 0;
  std::vector<std::pair<std::string, std::string>> options;
  CHECK(common::proto::decode_client_options(decoded.payload, &capabilities,
                                             &options));
  CHECK_EQ(capabilities, common::proto::kCapabilityCrossGroupReadLoose);
  CHECK_EQ(options.size(), size_t{0});

  // 可扩展 key/value 选项：往返保留。
  const std::string with_options = common::proto::encode_client_options(
      common::proto::kCapabilityCrossGroupReadLoose,
      {{"cross_group_read", "loose"}, {"future", "option"}});
  CHECK(common::proto::try_decode_frame(with_options, &decoded, &consumed,
                                        &error));
  CHECK(common::proto::decode_client_options(decoded.payload, &capabilities,
                                             &options));
  CHECK_EQ(options.size(), size_t{2});
  if (options.size() == 2) {
    CHECK_EQ(options[0].first, std::string("cross_group_read"));
    CHECK_EQ(options[0].second, std::string("loose"));
    CHECK_EQ(options[1].first, std::string("future"));
    CHECK_EQ(options[1].second, std::string("option"));
  }

  // 截断/越界必须失败。
  CHECK_FALSE(common::proto::decode_client_options(
                  with_options.substr(0, with_options.size() - 3), &capabilities,
                  &options));
}

TEST(Protocol, QueryRoundTrip) {
  const std::string sql = "SELECT * FROM t WHERE name = 'a;b';";
  common::proto::DecodedFrame decoded;
  size_t consumed = 0;
  std::string error;
  CHECK(common::proto::try_decode_frame(common::proto::encode_query(sql), &decoded, &consumed,
                                 &error));
  CHECK(decoded.type == common::proto::FrameType::kQuery);
  std::string out;
  CHECK(common::proto::decode_query(decoded.payload, &out));
  CHECK_EQ(out, sql);
}

TEST(Protocol, RowKeepsNullApartFromEmptyString) {
  std::vector<common::proto::ProtocolValue> values(3);
  values[0].is_null = true;  // NULL
  values[1].is_null = false; // 空串
  values[1].text = "";
  values[2].text = "NULL"; // 字符串 "NULL"（和真 NULL 不是一回事）

  common::proto::DecodedFrame decoded;
  size_t consumed = 0;
  std::string error;
  CHECK(common::proto::try_decode_frame(common::proto::encode_row(values), &decoded,
                                 &consumed, &error));
  CHECK(decoded.type == common::proto::FrameType::kRow);

  std::vector<common::proto::ProtocolValue> back;
  CHECK(common::proto::decode_row(decoded.payload, &back));
  CHECK_EQ(back.size(), size_t{3});
  if (back.size() == 3) {
    CHECK(back[0].is_null);
    CHECK(!back[1].is_null);
    CHECK_EQ(back[1].text, std::string());
    CHECK(!back[2].is_null);
    CHECK_EQ(back[2].text, std::string("NULL"));
  }
}

TEST(Protocol, ColumnsAndOkAndErrorRoundTrip) {
  const std::vector<std::string> columns{"id", "name with space", "年龄"};
  common::proto::DecodedFrame decoded;
  size_t consumed = 0;
  std::string error;
  CHECK(common::proto::try_decode_frame(common::proto::encode_columns(columns), &decoded,
                                 &consumed, &error));
  std::vector<std::string> back_columns;
  CHECK(common::proto::decode_columns(decoded.payload, &back_columns));
  CHECK_EQ(back_columns, columns);

  CHECK(common::proto::try_decode_frame(common::proto::encode_ok(42, true, true, "shop"),
                                 &decoded, &consumed, &error));
  uint64_t affected = 0;
  uint8_t flags = 0;
  std::string current_db;
  CHECK(common::proto::decode_ok(decoded.payload, &affected, &flags, &current_db));
  CHECK_EQ(affected, uint64_t{42});
  CHECK_EQ(int{flags & 1}, 1);
  CHECK_EQ(int{(flags >> 1) & 1}, 1); // is_write
  CHECK_EQ(current_db, std::string("shop"));

  common::proto::ErrorFrame error_frame;
  error_frame.code = 12;
  error_frame.message = "column not found: nope";
  error_frame.sql = "SELECT nope FROM t;";
  error_frame.begin_line = 1;
  error_frame.begin_column = 8;
  error_frame.end_line = 1;
  error_frame.end_column = 12;
  CHECK(common::proto::try_decode_frame(common::proto::encode_error(error_frame), &decoded,
                                 &consumed, &error));
  common::proto::ErrorFrame back;
  CHECK(common::proto::decode_error(decoded.payload, &back));
  CHECK_EQ(int{back.code}, 12);
  CHECK_EQ(back.message, error_frame.message);
  CHECK_EQ(back.sql, error_frame.sql);
  CHECK_EQ(back.begin_column, uint32_t{8});
  CHECK_EQ(back.end_column, uint32_t{12});
}

TEST(Protocol, ErrorFrameCarriesOptionalLeaderHint) {
  // 带 hint：客户端拿到"该去哪台"。
  common::proto::ErrorFrame error;
  error.code = 42;
  error.message = "not the leader";
  error.leader_hint = common::proto::LeaderHint{7, "10.0.0.7:5433"};

  common::proto::DecodedFrame frame;
  size_t consumed = 0;
  std::string decode_error;
  CHECK(common::proto::try_decode_frame(common::proto::encode_error(error),
                                        &frame, &consumed, &decode_error));
  common::proto::ErrorFrame decoded;
  CHECK(common::proto::decode_error(frame.payload, &decoded));
  CHECK_EQ(decoded.code, uint8_t{42});
  CHECK_EQ(decoded.message, std::string("not the leader"));
  CHECK_TRUE(decoded.leader_hint.has_value());
  if (decoded.leader_hint.has_value()) {
    CHECK_EQ(decoded.leader_hint->node_id, uint64_t{7});
    CHECK_EQ(decoded.leader_hint->endpoint, std::string("10.0.0.7:5433"));
  }

  // 不带 hint：编码结果与老格式一致（客户端拿到 nullopt）。
  common::proto::ErrorFrame plain;
  plain.message = "syntax error";
  CHECK(common::proto::try_decode_frame(common::proto::encode_error(plain),
                                        &frame, &consumed, &decode_error));
  common::proto::ErrorFrame plain_decoded;
  CHECK(common::proto::decode_error(frame.payload, &plain_decoded));
  CHECK_FALSE(plain_decoded.leader_hint.has_value());
}

TEST(Protocol, ErrorFrameWithoutTrailingHintIsStillReadable) {
  // 老服务端的格式：固定字段之后没有尾巴 —— 必须能解出来且没有 hint。
  std::string payload;
  payload.push_back(static_cast<char>(9)); // code
  common::proto::encode_error(common::proto::ErrorFrame{}); // 仅确保符号可用
  const auto append_bytes = [&payload](const std::string &bytes) {
    const uint32_t length = static_cast<uint32_t>(bytes.size());
    payload.push_back(static_cast<char>((length >> 0) & 0xff));
    payload.push_back(static_cast<char>((length >> 8) & 0xff));
    payload.push_back(static_cast<char>((length >> 16) & 0xff));
    payload.push_back(static_cast<char>((length >> 24) & 0xff));
    payload.append(bytes);
  };
  append_bytes("legacy error");
  append_bytes("");
  for (int i = 0; i < 4; ++i) {
    payload.append(4, '\0'); // 四个 u32 span 字段
  }

  common::proto::ErrorFrame decoded;
  CHECK(common::proto::decode_error(payload, &decoded));
  CHECK_EQ(decoded.code, uint8_t{9});
  CHECK_EQ(decoded.message, std::string("legacy error"));
  CHECK_FALSE(decoded.leader_hint.has_value());
}

TEST(Protocol, PartialAndPipelinedFrames) {
  const std::string a = common::proto::encode_query("SELECT 1;");
  const std::string b = common::proto::encode_simple(common::proto::FrameType::kPing);
  const std::string stream = a + b;

  common::proto::DecodedFrame decoded;
  size_t consumed = 0;
  std::string error;
  // 半帧：还没收全 -> false 且 consumed = 0，没有错误
  CHECK(!common::proto::try_decode_frame(stream.substr(0, 3), &decoded, &consumed,
                                  &error));
  CHECK_EQ(consumed, size_t{0});
  CHECK(error.empty());
  // 完整的流：一次取一帧（粘包由调用方推进 consumed）
  size_t offset = 0;
  CHECK(common::proto::try_decode_frame(stream.substr(offset), &decoded, &consumed,
                                 &error));
  CHECK(decoded.type == common::proto::FrameType::kQuery);
  offset += consumed;
  CHECK(common::proto::try_decode_frame(stream.substr(offset), &decoded, &consumed,
                                 &error));
  CHECK(decoded.type == common::proto::FrameType::kPing);
}

TEST(Protocol, MetaFramesRoundTrip) {
  // 请求帧：kind + 两个参数
  common::proto::DecodedFrame decoded;
  size_t consumed = 0;
  std::string error;
  CHECK(common::proto::try_decode_frame(
      common::proto::encode_meta(common::proto::MetaKind::kSchema, "shop", "users"), &decoded,
      &consumed, &error));
  CHECK(decoded.type == common::proto::FrameType::kMeta);
  uint8_t kind = 0;
  std::string arg1;
  std::string arg2;
  CHECK(common::proto::decode_meta(decoded.payload, &kind, &arg1, &arg2));
  CHECK_EQ(int{kind}, static_cast<int>(common::proto::MetaKind::kSchema));
  CHECK_EQ(arg1, std::string("shop"));
  CHECK_EQ(arg2, std::string("users"));

  // 回复帧：库列表
  std::vector<common::proto::MetaDatabase> databases(2);
  databases[0].name = "shop";
  databases[0].created_at = 1700000000;
  databases[0].table_count = 2;
  databases[0].is_current = true;
  databases[1].name = "other";
  const std::string payload = common::proto::encode_meta_databases(databases);
  CHECK(common::proto::try_decode_frame(common::proto::encode_meta_reply(payload), &decoded,
                                 &consumed, &error));
  CHECK(decoded.type == common::proto::FrameType::kMetaReply);
  std::string back_payload;
  CHECK(common::proto::decode_meta_reply(decoded.payload, &back_payload));
  std::vector<common::proto::MetaDatabase> back;
  CHECK(common::proto::decode_meta_databases(back_payload, &back));
  CHECK_EQ(back.size(), size_t{2});
  if (back.size() == 2) {
    CHECK_EQ(back[0].name, std::string("shop"));
    CHECK_EQ(back[0].created_at, int64_t{1700000000});
    CHECK_EQ(back[0].table_count, uint32_t{2});
    CHECK(back[0].is_current);
    CHECK_EQ(back[1].name, std::string("other"));
  }

  // 回复帧：表列表（含 NULL 主键用空串表示）
  std::vector<common::proto::MetaTable> tables(1);
  tables[0].name = "logs";
  tables[0].column_count = 2;
  tables[0].row_count = 7;
  const std::string tables_payload = common::proto::encode_meta_tables(tables);
  std::vector<common::proto::MetaTable> tables_back;
  CHECK(common::proto::decode_meta_tables(tables_payload, &tables_back));
  CHECK_EQ(tables_back.size(), size_t{1});
  if (tables_back.size() == 1) {
    CHECK_EQ(tables_back[0].name, std::string("logs"));
    CHECK_EQ(tables_back[0].column_count, uint32_t{2});
    CHECK_EQ(tables_back[0].row_count, uint64_t{7});
    CHECK(tables_back[0].primary_key.empty());
  }

  // 回复帧：schema（复用 TableSchema v1 序列化）
  sql::TableSchema schema(sql::Identifier("users"));
  schema.add_column(sql::Identifier("id"), sql::DataType::INT, true, false);
  const std::string schema_payload = common::proto::encode_meta_schema(schema);
  sql::TableSchema schema_back;
  CHECK(common::proto::decode_meta_schema(schema_payload, &schema_back));
  CHECK(schema_back.table_name() == sql::Identifier("users"));
  CHECK_EQ(schema_back.column_count(), size_t{1});
  CHECK(schema_back.has_primary_key());
}

TEST(Protocol, OversizedAndUnknownFramesAreRejected) {
  // 长度头超过上限：直接报协议错（不能让对端用长度头撑爆内存）
  std::string bogus;
  bogus.push_back(static_cast<char>(common::proto::FrameType::kQuery));
  const uint32_t too_big = common::proto::kMaxFrameSize + 1;
  for (int i = 0; i < 4; ++i) {
    bogus.push_back(static_cast<char>((too_big >> (8 * i)) & 0xFF));
  }
  common::proto::DecodedFrame decoded;
  size_t consumed = 0;
  std::string error;
  CHECK(!common::proto::try_decode_frame(bogus, &decoded, &consumed, &error));
  CHECK(!error.empty());

  // 未知帧类型
  std::string unknown;
  unknown.push_back(static_cast<char>(99));
  for (int i = 0; i < 4; ++i) {
    unknown.push_back(0);
  }
  error.clear();
  CHECK(!common::proto::try_decode_frame(unknown, &decoded, &consumed, &error));
  CHECK(!error.empty());
}
