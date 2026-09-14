// tests/test_server/test_remote_client_fake_server.cpp
//
// **不依赖监听端口**的客户端用例：用 socketpair 造一条"已建立的连接"，
// 一端跑一个极简假服务器（按协议应答），另一端是真正的
// `client::RemoteConnection`
// + 共用 REPL。
//
// 为什么需要它：受限环境里 `bind()` 会被拒（ServerE2E 只能 skip），而客户端的
// 解析逻辑（HELLO 必须被消费、OK 帧的 flags、META 帧）必须能在此验证 ——
// 这里抓出过一个真 bug：RemoteConnection 原来不读 HELLO，第一条语句就把
// HELLO 当成"不该出现的帧"而报连接断开。
#include "test_framework.h"

#include "client/connection.h"
#include "client/repl.h"
#include "server/protocol.h"

#include <sys/socket.h>
#include <unistd.h>

#include <cstdio>
#include <string>
#include <thread>
#include <vector>

namespace {

bool write_all(int fd, const std::string &data) {
  size_t sent = 0;
  while (sent < data.size()) {
    const ssize_t wrote = ::write(fd, data.data() + sent, data.size() - sent);
    if (wrote <= 0) {
      return false;
    }
    sent += static_cast<size_t>(wrote);
  }
  return true;
}

// 极简假服务器：只认几条固定语句 + META 请求
void serve_fake(int fd) {
  if (!write_all(fd, server::encode_hello(1, 0))) {
    return;
  }
  std::string in;
  while (true) {
    server::DecodedFrame frame;
    size_t consumed = 0;
    std::string error;
    if (!server::try_decode_frame(in, &frame, &consumed, &error)) {
      if (!error.empty()) {
        return;
      }
      char chunk[4096];
      const ssize_t got = ::read(fd, chunk, sizeof(chunk));
      if (got <= 0) {
        return;
      }
      in.append(chunk, static_cast<size_t>(got));
      continue;
    }
    in.erase(0, consumed);

    if (frame.type == server::FrameType::kBye) {
      return;
    }
    if (frame.type == server::FrameType::kMeta) {
      uint8_t kind = 0;
      std::string arg1;
      std::string arg2;
      if (!server::decode_meta(frame.payload, &kind, &arg1, &arg2)) {
        return;
      }
      std::string payload;
      if (static_cast<server::MetaKind>(kind) == server::MetaKind::kDatabases) {
        std::vector<server::MetaDatabase> dbs(1);
        dbs[0].name = "shop";
        dbs[0].table_count = 1;
        dbs[0].is_current = true;
        payload = server::encode_meta_databases(dbs);
      } else if (static_cast<server::MetaKind>(kind) ==
                 server::MetaKind::kTables) {
        std::vector<server::MetaTable> tables(1);
        tables[0].name = "users";
        tables[0].column_count = 3;
        tables[0].primary_key = "id";
        payload = server::encode_meta_tables(tables);
      } else {
        sql::TableSchema schema(sql::Identifier(arg2.empty() ? "users" : arg2));
        schema.add_column(sql::Identifier("id"), sql::DataType::INT, true,
                          false);
        payload = server::encode_meta_schema(schema);
      }
      if (!write_all(fd, server::encode_meta_reply(payload))) {
        return;
      }
      continue;
    }
    if (frame.type != server::FrameType::kQuery) {
      return;
    }
    std::string sql;
    if (!server::decode_query(frame.payload, &sql)) {
      return;
    }
    if (sql.find("nope") != std::string::npos) {
      server::ErrorFrame missing;
      missing.message = "table not found: nope (line 1:15)";
      missing.sql = sql;
      missing.begin_line = 1;
      missing.begin_column = 15;
      missing.end_line = 1;
      missing.end_column = 19;
      if (!write_all(fd, server::encode_error(missing))) {
        return;
      }
      continue;
    }
    if (sql.find("SELECT") != std::string::npos) {
      if (!write_all(fd, server::encode_columns({"id", "name"}))) {
        return;
      }
      std::vector<server::ProtocolValue> row(2);
      row[0].text = "1";
      row[1].is_null = true; // NULL 单元格
      if (!write_all(fd, server::encode_row(row))) {
        return;
      }
      // rows 结果也回 OK（affected = 行数，is_write = false）
      if (!write_all(fd, server::encode_ok(1, false, false, "shop"))) {
        return;
      }
      continue;
    }
    if (sql.find("BEGIN") != std::string::npos) {
      if (!write_all(fd, server::encode_ok(0, /*in_tx=*/true, false, "shop"))) {
        return;
      }
      continue;
    }
    if (sql.find("ROLLBACK") != std::string::npos) {
      if (!write_all(fd, server::encode_ok(0, false, false, "shop"))) {
        return;
      }
      continue;
    }
    // 其它（INSERT/DDL）：写语句，affected = 2
    if (!write_all(fd, server::encode_ok(2, false, true, "shop"))) {
      return;
    }
  }
}

} // namespace

// 回归：对端（服务器）已经没了以后客户端再发语句 —— `write()` 会送 SIGPIPE，
// 默认处置直接杀掉客户端进程（这里就是测试进程）。正确行为是**报连接断开**。
// 这跟"服务器被跑掉的客户端打死"是同一个坑的两面，见 common/net/socket_util.h。
TEST(FakeServer, WriteAfterServerIsGoneReportsConnectionLost) {
  int fds[2] = {-1, -1};
  CHECK_EQ(::socketpair(AF_UNIX, SOCK_STREAM, 0, fds), 0);
  // 握手要读到 HELLO，所以先替"服务器"把它写进管道，再让服务器那端消失
  CHECK(write_all(fds[0], server::encode_hello(server::kProtocolVersion, 0)));
  std::string error;
  std::unique_ptr<client::SqlConnection> connection =
      client::make_remote_from_fd(fds[1], &error);
  ::close(fds[0]); // 服务器那端没了
  CHECK(connection != nullptr); // 握手（HELLO）成功
  if (connection == nullptr) {
    CHECK_EQ(error, std::string()); // 失败信息：为什么握手没过
    return;
  }

  const client::Outcome outcome = connection->execute("SELECT 1");
  CHECK(!outcome.ok);
  CHECK_EQ(outcome.error_message, std::string("connection to server lost"));
}

TEST(FakeServer, RemoteClientConsumesHelloAndParsesFrames) {
  int fds[2] = {-1, -1};
  CHECK(::socketpair(AF_UNIX, SOCK_STREAM, 0, fds) == 0);
  if (fds[0] < 0) {
    return;
  }
  std::thread server_thread([&] { serve_fake(fds[0]); });

  std::string error;
  auto connection = client::make_remote_from_fd(fds[1], &error);
  CHECK(connection != nullptr); // 握手（HELLO）成功
  if (connection != nullptr) {
    // 写语句：affected + is_write
    auto inserted = connection->execute("INSERT INTO t VALUES (1)");
    CHECK(inserted.ok);
    CHECK(inserted.is_write);
    CHECK_EQ(inserted.affected_rows, uint64_t{2});

    // 行流：列名 + 行（含 NULL）+ 会话状态
    auto selected = connection->execute("SELECT id, name FROM t");
    CHECK(selected.ok);
    CHECK(selected.has_rows);
    CHECK_EQ(selected.columns.size(), size_t{2});
    CHECK_EQ(selected.rows.size(), size_t{1});
    if (selected.rows.size() == 1) {
      CHECK_EQ(selected.rows[0][0].text, std::string("1"));
      CHECK(selected.rows[0][1].is_null);
    }

    // 错误帧：消息 + span（客户端自己渲染 caret）
    auto missing = connection->execute("SELECT * FROM nope");
    CHECK(!missing.ok);
    CHECK(missing.error_message.find("nope") != std::string::npos);
    CHECK(sspan_valid(missing.error_span));

    // 事务状态来自 OK 帧的 flags
    CHECK(!connection->in_transaction());
    CHECK(connection->execute("BEGIN").ok);
    CHECK(connection->in_transaction());
    CHECK(connection->execute("ROLLBACK").ok);
    CHECK(!connection->in_transaction());

    // META 帧：\l / \dt / \d 的数据
    CHECK(connection->supports_metadata());
    const auto databases = connection->databases();
    CHECK_EQ(databases.size(), size_t{1});
    if (!databases.empty()) {
      CHECK_EQ(databases[0].name, std::string("shop"));
    }
    const auto tables = connection->tables("shop");
    CHECK_EQ(tables.size(), size_t{1});
    if (!tables.empty()) {
      CHECK_EQ(tables[0].name, std::string("users"));
      CHECK_EQ(tables[0].primary_key, std::string("id"));
    }
    const auto schema = connection->table_schema("users", "shop");
    CHECK(schema.has_value());
    if (schema.has_value()) {
      CHECK(schema->has_primary_key());
    }
  }

  ::close(fds[1]);
  if (server_thread.joinable()) {
    server_thread.join();
  }
}

TEST(FakeServer, SharedReplPrintsRowsAndNulls) {
  int fds[2] = {-1, -1};
  CHECK(::socketpair(AF_UNIX, SOCK_STREAM, 0, fds) == 0);
  if (fds[0] < 0) {
    return;
  }
  std::thread server_thread([&] { serve_fake(fds[0]); });

  std::string error;
  auto connection = client::make_remote_from_fd(fds[1], &error);
  CHECK(connection != nullptr);
  if (connection != nullptr) {
    FILE *sink = std::tmpfile();
    CHECK(sink != nullptr);
    client::ReplOptions repl;
    repl.out = sink;
    repl.err = sink;
    repl.colors = false;
    CHECK_EQ(client::run_text(*connection, "SELECT id, name FROM t;\n", repl),
             0);
    std::fflush(sink);
    std::rewind(sink);
    std::string output;
    char buffer[4096];
    size_t got = 0;
    while ((got = std::fread(buffer, 1, sizeof(buffer), sink)) > 0) {
      output.append(buffer, got);
    }
    std::fclose(sink);
    CHECK(output.find("id") != std::string::npos);
    CHECK(output.find("NULL") != std::string::npos);
    CHECK(output.find("(1 row)") != std::string::npos);
  }

  ::close(fds[1]);
  if (server_thread.joinable()) {
    server_thread.join();
  }
}
