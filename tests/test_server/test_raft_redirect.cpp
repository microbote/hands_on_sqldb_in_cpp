// NotLeader + leader hint，两端各测一半，都不需要 bind()：
//
//   服务端半边：一个"会把 leader_hint 报出来"的 KVStore + 真 Server（socketpair
//   接入，不监听）+ 真客户端 —— 验证 server 的预检查、ERROR 帧编码与能力位；
//   客户端半边：假服务端 1 回 NotLeader + hint，REPL 自动 dial 到假服务端 2
//   重试 —— 验证"换节点重试一次"的完整路径。

#include "test_framework.h"

#include "client/connection.h"
#include "client/repl.h"
#include "common/proto/protocol.h"
#include "server/config.h"
#include "server/logger.h"
#include "server/server.h"
#include "storage/mock_engine/mock_engine.h"

#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <cstdio>
#include <expected>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <utility>

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

// 读一条完整帧（不足就继续读）；EOF/协议错返回 nullopt。
std::optional<common::proto::DecodedFrame> read_frame(int fd,
                                                      std::string *buffer) {
  while (true) {
    common::proto::DecodedFrame frame;
    size_t consumed = 0;
    std::string error;
    if (common::proto::try_decode_frame(*buffer, &frame, &consumed, &error)) {
      buffer->erase(0, consumed);
      return frame;
    }
    if (!error.empty()) {
      return std::nullopt;
    }
    char chunk[4096];
    const ssize_t got = ::read(fd, chunk, sizeof(chunk));
    if (got <= 0) {
      return std::nullopt;
    }
    buffer->append(chunk, static_cast<size_t>(got));
  }
}

// ---- 服务端半边 ----

// 一个把所有活都转给内层 store、但永远说"leader 在别处"的 KVStore。
class RedirectingStore final : public kv::KVStore {
public:
  RedirectingStore(std::shared_ptr<kv::KVStore> inner, kv::LeaderHint hint)
      : inner_(std::move(inner)), hint_(std::move(hint)) {}

  kv::Status open(const kv::DatabaseOptions &options) override {
    return inner_->open(options);
  }
  kv::Status close() override { return inner_->close(); }
  bool is_open() const override { return inner_->is_open(); }
  std::shared_ptr<kv::KVEngine> connect() override { return inner_->connect(); }
  std::string name() const override { return "RedirectingStore"; }
  void flush() override { inner_->flush(); }
  std::string stats() const override { return inner_->stats(); }
  bool write_slot_held() const override { return inner_->write_slot_held(); }
  kv::Status write_batch(const kv::WriteBatch &batch) override {
    return inner_->write_batch(batch);
  }
  std::unique_ptr<kv::Iterator>
  new_iterator(const kv::KeyRange &range) override {
    return inner_->new_iterator(range);
  }
  std::optional<kv::LeaderHint> leader_hint() override { return hint_; }

private:
  std::shared_ptr<kv::KVStore> inner_;
  kv::LeaderHint hint_;
};

TEST(RaftRedirect, ServerAnswersNotLeaderWithTheLeaderEndpoint) {
  int fds[2] = {-1, -1};
  CHECK_EQ(::socketpair(AF_UNIX, SOCK_STREAM, 0, fds), 0);

  auto logger = server::Logger::create("error", "");
  CHECK_TRUE(logger.has_value());
  if (!logger.has_value()) {
    return;
  }
  auto local = std::make_shared<kv::MockStore>();
  CHECK_EQ(local->open(kv::DatabaseOptions{}), kv::Status::OK);
  auto store = std::make_shared<RedirectingStore>(
      local, kv::LeaderHint{9, "10.0.0.9:5433"});

  server::ServerConfig config;
  config.set("storage.engine", "mock");
  config.set("storage.path", "mock://redirect");
  config.set("server.listen", "127.0.0.1:0");
  server::Server server(config, store, *logger);
  server.attach_connection(fds[1]); // 不 listen：只挂一条已建立的连接
  std::thread server_thread([&] { server.run(); });

  std::string error;
  auto connection = client::make_remote_from_fd(fds[0], &error);
  CHECK_TRUE(connection != nullptr);
  if (connection != nullptr) {
    // 读语句：照样先被"这台不是 leader"拦下。
    // 语法必须能过（预检查在 parse 之后、执行之前），所以用真实表名。
    const client::Outcome read = connection->execute("SELECT * FROM t");
    CHECK_FALSE(read.ok);
    CHECK_EQ(read.error_code,
             static_cast<uint8_t>(session::SessionErrorCode::NOT_LEADER));
    CHECK_EQ(read.redirect_node_id, uint64_t{9});
    CHECK_EQ(read.redirect_endpoint, std::string("10.0.0.9:5433"));
    CHECK(read.error_message.find("10.0.0.9:5433") != std::string::npos);

    // 写语句也一样（P1 只有一个 group，整条语句都不该在这台执行）。
    const client::Outcome write =
        connection->execute("INSERT INTO t VALUES (1)");
    CHECK_FALSE(write.ok);
    CHECK_EQ(write.redirect_endpoint, std::string("10.0.0.9:5433"));
  }

  connection.reset(); // 关客户端这一端，服务端连接协程才会退出
  server.stop();
  if (server_thread.joinable()) {
    server_thread.join();
  }
}

TEST(RaftRedirect, ServerWithoutHintExecutesNormally) {
  int fds[2] = {-1, -1};
  CHECK_EQ(::socketpair(AF_UNIX, SOCK_STREAM, 0, fds), 0);

  auto logger = server::Logger::create("error", "");
  CHECK_TRUE(logger.has_value());
  if (!logger.has_value()) {
    return;
  }
  auto store = std::make_shared<kv::MockStore>();
  CHECK_EQ(store->open(kv::DatabaseOptions{}), kv::Status::OK);

  server::ServerConfig config;
  config.set("storage.engine", "mock");
  config.set("storage.path", "mock://no-redirect");
  config.set("server.listen", "127.0.0.1:0");
  server::Server server(config, store, *logger);
  server.attach_connection(fds[1]);
  std::thread server_thread([&] { server.run(); });

  std::string error;
  auto connection = client::make_remote_from_fd(fds[0], &error);
  CHECK_TRUE(connection != nullptr);
  if (connection != nullptr) {
    const client::Outcome outcome =
        connection->execute("CREATE DATABASE shop");
    CHECK_TRUE(outcome.ok); // 单机存储没有 leader 概念
    CHECK_TRUE(outcome.redirect_endpoint.empty());
  }

  connection.reset();
  server.stop();
  if (server_thread.joinable()) {
    server_thread.join();
  }
}

// ---- 客户端半边 ----

// 假服务端 1：握手后对第一条语句回 NotLeader + hint，然后等连接关闭。
void serve_not_leader(int fd, std::atomic<int> *queries) {
  if (!write_all(fd, common::proto::encode_hello(
                         common::proto::kProtocolVersion,
                         common::proto::kCapabilityLeaderHint))) {
    return;
  }
  std::string buffer;
  while (true) {
    auto frame = read_frame(fd, &buffer);
    if (!frame.has_value()) {
      return;
    }
    if (frame->type != common::proto::FrameType::kQuery) {
      continue;
    }
    queries->fetch_add(1);
    common::proto::ErrorFrame error;
    error.code = static_cast<uint8_t>(session::SessionErrorCode::NOT_LEADER);
    error.message = "not the leader for this group; leader is at 10.9.9.9:5433";
    error.leader_hint = common::proto::LeaderHint{2, "10.9.9.9:5433"};
    if (!write_all(fd, common::proto::encode_error(error))) {
      return;
    }
  }
}

// 假服务端 2：握手后对第一条语句回一行结果 + OK。
void serve_rows(int fd, std::atomic<int> *queries) {
  if (!write_all(fd, common::proto::encode_hello(
                         common::proto::kProtocolVersion,
                         common::proto::kCapabilityLeaderHint))) {
    return;
  }
  std::string buffer;
  while (true) {
    auto frame = read_frame(fd, &buffer);
    if (!frame.has_value()) {
      return;
    }
    if (frame->type != common::proto::FrameType::kQuery) {
      continue;
    }
    queries->fetch_add(1);
    std::vector<common::proto::ProtocolValue> row(1);
    row[0].text = "1";
    if (!write_all(fd, common::proto::encode_columns({"id"})) ||
        !write_all(fd, common::proto::encode_row(row)) ||
        !write_all(fd, common::proto::encode_ok(1, false, false, ""))) {
      return;
    }
  }
}

TEST(RaftRedirect, ReplReconnectsToTheHintedLeaderAndRetries) {
  int first[2] = {-1, -1};
  int second[2] = {-1, -1};
  CHECK_EQ(::socketpair(AF_UNIX, SOCK_STREAM, 0, first), 0);
  CHECK_EQ(::socketpair(AF_UNIX, SOCK_STREAM, 0, second), 0);

  std::atomic<int> first_queries{0};
  std::atomic<int> second_queries{0};
  std::thread first_thread([&] { serve_not_leader(first[0], &first_queries); });
  std::thread second_thread([&] { serve_rows(second[0], &second_queries); });

  std::atomic<int> dials{0};
  client::RemoteOptions options;
  options.host = "127.0.0.1";
  options.port = "5433";
  // 第一次 dial = 连初始节点；重定向时再 dial 一次 = 连 leader。
  options.dialer = [&](const std::string &, const std::string &)
      -> std::expected<int, std::string> {
    const int call = dials.fetch_add(1);
    return call == 0 ? ::dup(first[1]) : ::dup(second[1]);
  };

  std::string error;
  auto connection = client::make_remote(options, &error);
  CHECK_TRUE(connection != nullptr);
  if (connection != nullptr) {
    FILE *sink = std::tmpfile();
    CHECK(sink != nullptr);
    client::ReplOptions repl;
    repl.out = sink;
    repl.err = sink;
    const int failures =
        client::run_text(*connection, "SELECT id FROM t;\n", repl);
    CHECK_EQ(failures, 0); // 重定向成功 + 重试成功
    CHECK_EQ(first_queries.load(), 1);
    CHECK_EQ(second_queries.load(), 1);
    CHECK_EQ(dials.load(), 2);
    std::fclose(sink);
  }

  // 顺序要紧：先销毁连接（它持有 dialer 返回的 dup），再关测试自己那端，
  // 否则假服务端读不到 EOF，join 会一直等下去。
  connection.reset();
  ::close(first[1]);
  ::close(second[1]);
  if (first_thread.joinable()) {
    first_thread.join();
  }
  if (second_thread.joinable()) {
    second_thread.join();
  }
}

TEST(RaftRedirect, ReplDoesNotRetryInsideATransaction) {
  int first[2] = {-1, -1};
  CHECK_EQ(::socketpair(AF_UNIX, SOCK_STREAM, 0, first), 0);

  std::atomic<int> queries{0};
  // BEGIN 成功（进事务），其余语句回 NotLeader + hint。
  std::thread first_thread([&] {
    if (!write_all(first[0], common::proto::encode_hello(
                                 common::proto::kProtocolVersion,
                                 common::proto::kCapabilityLeaderHint))) {
      return;
    }
    std::string buffer;
    while (true) {
      auto frame = read_frame(first[0], &buffer);
      if (!frame.has_value()) {
        return;
      }
      if (frame->type != common::proto::FrameType::kQuery) {
        continue;
      }
      queries.fetch_add(1);
      std::string sql;
      if (!common::proto::decode_query(frame->payload, &sql)) {
        return;
      }
      if (sql.find("BEGIN") != std::string::npos) {
        if (!write_all(first[0], common::proto::encode_ok(0, true, false, ""))) {
          return;
        }
        continue;
      }
      common::proto::ErrorFrame error;
      error.code =
          static_cast<uint8_t>(session::SessionErrorCode::NOT_LEADER);
      error.message = "not the leader for this group";
      error.leader_hint = common::proto::LeaderHint{2, "10.9.9.9:5433"};
      if (!write_all(first[0], common::proto::encode_error(error))) {
        return;
      }
    }
  });

  std::atomic<int> dials{0};
  client::RemoteOptions options;
  options.host = "127.0.0.1";
  options.port = "5433";
  options.dialer = [&](const std::string &, const std::string &)
      -> std::expected<int, std::string> {
    dials.fetch_add(1);
    return ::dup(first[1]);
  };

  std::string error;
  auto connection = client::make_remote(options, &error);
  CHECK_TRUE(connection != nullptr);
  if (connection != nullptr) {
    FILE *sink = std::tmpfile();
    CHECK(sink != nullptr);
    client::ReplOptions repl;
    repl.out = sink;
    repl.err = sink;
    const int failures =
        client::run_text(*connection, "BEGIN;\nSELECT 1;\n", repl);
    CHECK_EQ(failures, 1); // 事务里的 NotLeader 直接报错，不换节点
    CHECK_TRUE(connection->in_transaction());
    CHECK_EQ(queries.load(), 2); // BEGIN + SELECT
    CHECK_EQ(dials.load(), 1);   // 没有第二次 dial
    std::fclose(sink);
  }

  connection.reset();
  ::close(first[1]);
  if (first_thread.joinable()) {
    first_thread.join();
  }
}

} // namespace
