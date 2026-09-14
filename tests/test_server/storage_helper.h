// tests/test_server/storage_helper.h
//
// 测试用的阻塞客户端 + 一个跑在后台线程的 Server。
#pragma once

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <chrono>
#include <functional>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "common/proto/protocol.h"
#include "server/server.h"
#include "session/session.h"
#include "storage/kv_engine/kv_factory.h"
#include "test_framework.h"

namespace srvtest {

// ---- 后台 Server：mock 引擎 + 内核挑端口 ----
class RunningServer {
public:
  // tweak：构造期调整配置（比如把 execution.read_threads 调大）
  using ConfigTweak = std::function<void(server::ServerConfig &)>;

  explicit RunningServer(bool default_db = true, ConfigTweak tweak = {}) {
    config_.set("storage.engine", "mock");
    config_.set("storage.path", "mock://server-test");
    config_.set("server.listen", "127.0.0.1:0"); // 端口 0 = 内核挑
    if (default_db) {
      config_.set("session.default_database", "shop");
    }
    if (tweak) {
      tweak(config_);
    }
    kv::DatabaseOptions options;
    options.set_path(config_.path());
    store_ = kv::open_store(kv::EngineType::MOCK, options);
    server_ = std::make_unique<server::Server>(config_, store_);
  }

  ~RunningServer() { stop(); }

  bool start() {
    if (auto ok = server_->listen(); !ok.has_value()) {
      last_error_ = ok.error();
      return false;
    }
    port_ = server_->port();
    // 建库建表（服务端进程自己的"初始化"；客户端连上时 default_database
    // 已存在）
    bootstrap_store();
    thread_ = std::thread([this] { server_->run(); });
    return wait_until_connectable();
  }

  const std::string &last_error() const { return last_error_; }

  void stop() {
    if (server_ != nullptr) {
      server_->stop();
    }
    if (thread_.joinable()) {
      thread_.join();
    }
  }

  int port() const { return port_; }
  server::Server &running() { return *server_; }

private:
  // 等监听真正可用：反复尝试连接（连上就立刻关掉）
  bool wait_until_connectable() {
    for (int i = 0; i < 400; ++i) {
      const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
      if (fd < 0) {
        return false;
      }
      sockaddr_in addr{};
      addr.sin_family = AF_INET;
      addr.sin_port = htons(static_cast<uint16_t>(port_));
      ::inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
      const int rc =
          ::connect(fd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr));
      ::close(fd);
      if (rc == 0) {
        return true;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return false;
  }

  void bootstrap_store() {
    // 直接用一条连接把库/表建好（不经过网络）
    auto session_conn = store_->connect();
    session::Session session(session_conn);
    kv::ByteValue ignore;
    (void)ignore;
    (void)session.execute("CREATE DATABASE shop");
    (void)session.execute("USE shop");
    (void)session.execute(
        "CREATE TABLE users (id INT PRIMARY KEY, name VARCHAR(16), age INT)");
  }

  server::ServerConfig config_;
  std::shared_ptr<kv::KVStore> store_;
  std::unique_ptr<server::Server> server_;
  std::thread thread_;
  int port_ = 0;
  std::string last_error_;
};

// ---- 阻塞式测试客户端（用同一套 protocol 编解码）----
class Client {
public:
  explicit Client(int port) {
    fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
    CHECK(fd_ >= 0);
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(static_cast<uint16_t>(port));
    ::inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
    CHECK(::connect(fd_, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) ==
          0);
    set_timeout();
    handshake();
  }

  ~Client() {
    if (fd_ >= 0) {
      ::close(fd_);
    }
  }

  Client(const Client &) = delete;
  Client &operator=(const Client &) = delete;

  // 一条语句的完整响应（读到 OK/ERROR 为止）
  struct Response {
    bool ok = false;
    bool in_transaction = false;
    std::string current_database;
    std::vector<std::string> columns;
    std::vector<std::vector<common::proto::ProtocolValue>> rows;
    uint64_t affected_rows = 0;
    common::proto::ErrorFrame error;
  };

  Response query(const std::string &sql) {
    Response response;
    CHECK(send_all(common::proto::encode_query(sql)));
    while (true) {
      auto frame = next_frame();
      CHECK(frame.has_value());
      if (!frame.has_value()) {
        break;
      }
      if (frame->type == common::proto::FrameType::kColumns) {
        CHECK(common::proto::decode_columns(frame->payload, &response.columns));
      } else if (frame->type == common::proto::FrameType::kRow) {
        std::vector<common::proto::ProtocolValue> row;
        CHECK(common::proto::decode_row(frame->payload, &row));
        response.rows.push_back(std::move(row));
      } else if (frame->type == common::proto::FrameType::kOk) {
        uint8_t flags = 0;
        std::string current_db;
        CHECK(common::proto::decode_ok(frame->payload, &response.affected_rows, &flags,
                                &current_db));
        response.in_transaction = (flags & 1) != 0;
        response.current_database = current_db;
        response.ok = true;
        break;
      } else if (frame->type == common::proto::FrameType::kError) {
        CHECK(common::proto::decode_error(frame->payload, &response.error));
        response.ok = false;
        break;
      } else {
        CHECK(false); // 别的帧不该出现在这里
        break;
      }
    }
    return response;
  }

private:
  void set_timeout() {
    timeval timeout{5, 0}; // 5s：测试不挂死
    ::setsockopt(fd_, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
  }

  void handshake() {
    // 读 HELLO
    auto hello = next_frame();
    CHECK(hello.has_value());
    if (hello.has_value()) {
      CHECK(hello->type == common::proto::FrameType::kHello);
      uint16_t proto = 0;
      uint16_t version = 0;
      uint32_t caps = 0;
      CHECK(common::proto::decode_hello(hello->payload, &proto, &version, &caps));
      CHECK_EQ(proto, common::proto::kProtocolVersion);
    }
  }
  bool send_all(const std::string &data) {
    size_t sent = 0;
    while (sent < data.size()) {
      const ssize_t wrote =
          ::write(fd_, data.data() + sent, data.size() - sent);
      if (wrote <= 0) {
        return false;
      }
      sent += static_cast<size_t>(wrote);
    }
    return true;
  }

  std::optional<common::proto::DecodedFrame> next_frame() {
    while (true) {
      common::proto::DecodedFrame frame;
      size_t consumed = 0;
      std::string error;
      if (common::proto::try_decode_frame(in_, &frame, &consumed, &error)) {
        in_.erase(0, consumed);
        return frame;
      }
      if (!error.empty()) {
        return std::nullopt;
      }
      char chunk[8192];
      const ssize_t got = ::read(fd_, chunk, sizeof(chunk));
      if (got <= 0) {
        return std::nullopt;
      }
      in_.append(chunk, static_cast<size_t>(got));
    }
  }

  int fd_ = -1;
  std::string in_;
};

} // namespace srvtest
