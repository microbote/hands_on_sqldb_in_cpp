// tests/test_server/test_protocol.cpp
//
// 协议编解码：往返、NULL 与空串必须可区分、半帧/粘包、超长帧与未知类型被拒。
#include "test_framework.h"

#include "server/protocol.h"

#include <string>
#include <vector>

TEST(Protocol, HelloRoundTrip) {
  const std::string frame = server::encode_hello(7, 0x3);
  server::DecodedFrame decoded;
  size_t consumed = 0;
  std::string error;
  CHECK(server::try_decode_frame(frame, &decoded, &consumed, &error));
  CHECK_EQ(consumed, frame.size());
  CHECK(decoded.type == server::FrameType::kHello);

  uint16_t proto = 0;
  uint16_t version = 0;
  uint32_t caps = 0;
  CHECK(server::decode_hello(decoded.payload, &proto, &version, &caps));
  CHECK_EQ(proto, server::kProtocolVersion);
  CHECK_EQ(version, uint16_t{7});
  CHECK_EQ(caps, uint32_t{3});
}

TEST(Protocol, QueryRoundTrip) {
  const std::string sql = "SELECT * FROM t WHERE name = 'a;b';";
  server::DecodedFrame decoded;
  size_t consumed = 0;
  std::string error;
  CHECK(server::try_decode_frame(server::encode_query(sql), &decoded, &consumed,
                                 &error));
  CHECK(decoded.type == server::FrameType::kQuery);
  std::string out;
  CHECK(server::decode_query(decoded.payload, &out));
  CHECK_EQ(out, sql);
}

TEST(Protocol, RowKeepsNullApartFromEmptyString) {
  std::vector<server::ProtocolValue> values(3);
  values[0].is_null = true;  // NULL
  values[1].is_null = false; // 空串
  values[1].text = "";
  values[2].text = "NULL"; // 字符串 "NULL"（和真 NULL 不是一回事）

  server::DecodedFrame decoded;
  size_t consumed = 0;
  std::string error;
  CHECK(server::try_decode_frame(server::encode_row(values), &decoded,
                                 &consumed, &error));
  CHECK(decoded.type == server::FrameType::kRow);

  std::vector<server::ProtocolValue> back;
  CHECK(server::decode_row(decoded.payload, &back));
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
  server::DecodedFrame decoded;
  size_t consumed = 0;
  std::string error;
  CHECK(server::try_decode_frame(server::encode_columns(columns), &decoded,
                                 &consumed, &error));
  std::vector<std::string> back_columns;
  CHECK(server::decode_columns(decoded.payload, &back_columns));
  CHECK_EQ(back_columns, columns);

  CHECK(server::try_decode_frame(server::encode_ok(42), &decoded, &consumed,
                                 &error));
  uint64_t affected = 0;
  CHECK(server::decode_ok(decoded.payload, &affected));
  CHECK_EQ(affected, uint64_t{42});

  server::ErrorFrame error_frame;
  error_frame.code = 12;
  error_frame.message = "column not found: nope";
  error_frame.sql = "SELECT nope FROM t;";
  error_frame.begin_line = 1;
  error_frame.begin_column = 8;
  error_frame.end_line = 1;
  error_frame.end_column = 12;
  CHECK(server::try_decode_frame(server::encode_error(error_frame), &decoded,
                                 &consumed, &error));
  server::ErrorFrame back;
  CHECK(server::decode_error(decoded.payload, &back));
  CHECK_EQ(int{back.code}, 12);
  CHECK_EQ(back.message, error_frame.message);
  CHECK_EQ(back.sql, error_frame.sql);
  CHECK_EQ(back.begin_column, uint32_t{8});
  CHECK_EQ(back.end_column, uint32_t{12});
}

TEST(Protocol, PartialAndPipelinedFrames) {
  const std::string a = server::encode_query("SELECT 1;");
  const std::string b = server::encode_simple(server::FrameType::kPing);
  const std::string stream = a + b;

  server::DecodedFrame decoded;
  size_t consumed = 0;
  std::string error;
  // 半帧：还没收全 -> false 且 consumed = 0，没有错误
  CHECK(!server::try_decode_frame(stream.substr(0, 3), &decoded, &consumed,
                                  &error));
  CHECK_EQ(consumed, size_t{0});
  CHECK(error.empty());
  // 完整的流：一次取一帧（粘包由调用方推进 consumed）
  size_t offset = 0;
  CHECK(server::try_decode_frame(stream.substr(offset), &decoded, &consumed,
                                 &error));
  CHECK(decoded.type == server::FrameType::kQuery);
  offset += consumed;
  CHECK(server::try_decode_frame(stream.substr(offset), &decoded, &consumed,
                                 &error));
  CHECK(decoded.type == server::FrameType::kPing);
}

TEST(Protocol, OversizedAndUnknownFramesAreRejected) {
  // 长度头超过上限：直接报协议错（不能让对端用长度头撑爆内存）
  std::string bogus;
  bogus.push_back(static_cast<char>(server::FrameType::kQuery));
  const uint32_t too_big = server::kMaxFrameSize + 1;
  for (int i = 0; i < 4; ++i) {
    bogus.push_back(static_cast<char>((too_big >> (8 * i)) & 0xFF));
  }
  server::DecodedFrame decoded;
  size_t consumed = 0;
  std::string error;
  CHECK(!server::try_decode_frame(bogus, &decoded, &consumed, &error));
  CHECK(!error.empty());

  // 未知帧类型
  std::string unknown;
  unknown.push_back(static_cast<char>(99));
  for (int i = 0; i < 4; ++i) {
    unknown.push_back(0);
  }
  error.clear();
  CHECK(!server::try_decode_frame(unknown, &decoded, &consumed, &error));
  CHECK(!error.empty());
}
