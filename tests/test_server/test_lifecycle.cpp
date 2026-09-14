// tests/test_server/test_lifecycle.cpp
//
// 连接生命周期：空闲超时 / 事务空闲超时 / 优雅退出 / metrics。
//
// 这里用 `socketpair + Server::attach_connection()` 造连接（不占端口，
// 受限沙箱里 `bind()` 被拒时也能跑）。调通过程中抓出的三个真 bug（都在
// server/ 侧修掉了）：
//   1. `attach_connection()` 没把 fd 设成非阻塞 -> 一次 read 就把事件循环
//      线程堵死（定时器、其它连接全停）；
//   2. 只 attach、没 `listen()` 时 `listen_fd_ == -1`，`accept_loop` 的
//      `WaitFd{fd<0}` 立刻 ready -> 协程在 while 里**忙等不让出**，整条事件
//      循环被饿死（现象：连 SELECT 都拿不到回复，定时器也不走）；
//   3. 对端已经消失时 `write()` 送的 SIGPIPE 按默认处置**杀掉整个进程**
//      （现象：测试进程 exit=141）。修法见 common/net/socket_util.h。
// 下面 4 个用例把这三条都钉住了。

#include "test_framework.h"

#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <chrono>
#include <memory>
#include <string>
#include <thread>

#include "client/connection.h"
#include "common/proto/protocol.h"
#include "server/server.h"
#include "storage/kv_engine/kv_factory.h"

namespace {

// 直接从 fd 上读一帧（带超时）：测试里手动跟服务器对话时用
bool read_frame(int fd, common::proto::DecodedFrame *out, int timeout_ms) {
  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
  std::string buffer;
  while (true) {
    size_t consumed = 0;
    std::string error;
    if (common::proto::try_decode_frame(buffer, out, &consumed, &error)) {
      return true;
    }
    if (!error.empty()) {
      return false;
    }
    const auto now = std::chrono::steady_clock::now();
    if (now >= deadline) {
      return false;
    }
    pollfd entry{};
    entry.fd = fd;
    entry.events = POLLIN;
    const int wait_ms = static_cast<int>(
        std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now)
            .count());
    if (::poll(&entry, 1, wait_ms) <= 0) {
      return false;
    }
    char chunk[4096];
    const ssize_t got = ::read(fd, chunk, sizeof(chunk));
    if (got <= 0) {
      return false;
    }
    buffer.append(chunk, static_cast<size_t>(got));
  }
}

// 起一个"服务器 + 一条已建立连接"（不起监听，靠 attach_connection 挂进去）。
// 所有权：`server_fd` 在 attach 之后归 Server；`peer_fd` 在造出 connection
// 之后归 connection（两者都不再由 Fixture 关，避免同一个 fd 被关两次 ——
// 那会把事件循环里刚拿到的 fd 关掉，症状是"莫名其妙的连接断开"）。
struct Fixture {
  server::ServerConfig config;
  std::shared_ptr<kv::KVStore> store;
  std::unique_ptr<server::Server> server;
  std::thread thread;
  int server_fd = -1;
  int peer_fd = -1;
  std::unique_ptr<client::SqlConnection> connection;

  // 配置 + 存储 + socketpair（还没 attach、还没跑事件循环）
  bool prepare(int64_t idle_ms, int64_t idle_in_tx_ms) {
    config.set("storage.engine", "mock");
    config.set("storage.path", "mock://lifecycle-test");
    config.set("server.listen", "127.0.0.1:0");
    config.set("session.default_database", "shop");
    config.set("server.idle_timeout_ms", sql::Value(idle_ms));
    config.set("server.idle_in_transaction_timeout_ms",
               sql::Value(idle_in_tx_ms));
    config.set("server.log_level", "error"); // 测试里安静点

    kv::DatabaseOptions options;
    options.set_path(config.path());
    store = kv::open_store(kv::EngineType::MOCK, options);
    if (store == nullptr) {
      return false;
    }
    // 建库建表（服务端进程自己的初始化）
    {
      session::Session session(store->connect());
      (void)session.execute("CREATE DATABASE shop");
      (void)session.execute("USE shop");
      (void)session.execute("CREATE TABLE t (id INT PRIMARY KEY, v INT)");
    }

    int fds[2] = {-1, -1};
    if (::socketpair(AF_UNIX, SOCK_STREAM, 0, fds) != 0) {
      return false;
    }
    server = std::make_unique<server::Server>(config, store);
    server_fd = fds[0];
    peer_fd = fds[1];
    return true;
  }

  void attach_and_run() {
    server->attach_connection(server_fd);
    server_fd = -1;
    thread = std::thread([this] { server->run(); });
  }

  // 用 peer_fd 造一条 RemoteConnection（含 HELLO 握手）
  bool connect_client() {
    // 客户端这一端也设个读超时：不然"服务器不回"时测试会永久阻塞
    timeval timeout{1, 0};
    ::setsockopt(peer_fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    std::string error;
    const int fd = peer_fd;
    peer_fd = -1; // 无论成败都归 connection（握手失败时它已经关掉了）
    connection = client::make_remote_from_fd(fd, &error);
    return connection != nullptr;
  }

  bool start(int64_t idle_ms, int64_t idle_in_tx_ms) {
    if (!prepare(idle_ms, idle_in_tx_ms)) {
      return false;
    }
    attach_and_run();
    if (!connect_client()) {
      return false;
    }
    return true;
  }

  // 回归用：客户端那端**先关掉**再 attach —— 服务器写 HELLO 时对端已经没了
  bool attach_with_peer_already_gone(int64_t idle_ms, int64_t idle_in_tx_ms) {
    if (!prepare(idle_ms, idle_in_tx_ms)) {
      return false;
    }
    ::close(peer_fd);
    peer_fd = -1;
    attach_and_run();
    return true;
  }

  ~Fixture() {
    if (server != nullptr) {
      server->request_shutdown(); // 优雅退出（幂等）
      server->stop();
    }
    if (thread.joinable()) {
      thread.join();
    }
    if (peer_fd >= 0) {
      ::close(peer_fd); // 还在自己手里的那一端
    }
  }

  // 等连接被服务器断开（读不到东西 / 收到错误帧）
  bool wait_closed(int timeout_ms) {
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::milliseconds(timeout_ms);
    while (std::chrono::steady_clock::now() < deadline) {
      if (server->metrics().idle_timeouts.load() > 0) {
        // 再看连接是不是真的不可用了：发一条语句应当失败
        // （客户端有 1s 读超时，所以这里不会永久阻塞）
        auto outcome = connection->execute("SELECT id FROM t");
        return !outcome.ok;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    return false;
  }
};

} // namespace

TEST(ServerLifecycle, IdleTimeoutClosesTheConnection) {
  Fixture fixture;
  CHECK(fixture.start(/*idle_ms=*/100, /*idle_in_tx_ms=*/0));
  CHECK(fixture.connection->execute("SELECT id FROM t").ok);

  // 什么都不发：100ms 后服务器关读方向 -> 回 ERROR -> 断开
  CHECK(fixture.wait_closed(3000));
  CHECK_EQ(fixture.server->metrics().idle_timeouts.load(), uint64_t{1});
}

// 空闲看门狗到点后，服务器**不等客户端再发东西**就自己回 ERROR 帧。
// 这条同时钉住"用 `shutdown(SHUT_RD)` 能把阻塞在 `poll` 的读协程叫醒"——
// 第七节原来怀疑 AF_UNIX 上 `shutdown` 叫不醒，实测是可以的。
TEST(ServerLifecycle, IdleTimeoutSendsErrorFrameOnItsOwn) {
  Fixture fixture;
  CHECK(fixture.prepare(/*idle_ms=*/100, /*idle_in_tx_ms=*/0));
  fixture.attach_and_run();
  CHECK(fixture.peer_fd >= 0);

  common::proto::DecodedFrame hello;
  CHECK(read_frame(fixture.peer_fd, &hello, 2000));
  CHECK_EQ(static_cast<int>(hello.type),
           static_cast<int>(common::proto::FrameType::kHello));

  // 从这里开始一个字都不发：等服务器自己回 ERROR
  common::proto::DecodedFrame frame;
  CHECK(read_frame(fixture.peer_fd, &frame, 3000));
  CHECK_EQ(static_cast<int>(frame.type),
           static_cast<int>(common::proto::FrameType::kError));
  common::proto::ErrorFrame error;
  CHECK(common::proto::decode_error(frame.payload, &error));
  CHECK(error.message.find("idle") != std::string::npos);
  CHECK_EQ(fixture.server->metrics().idle_timeouts.load(), uint64_t{1});
}

TEST(ServerLifecycle, IdleInTransactionTimeoutRollsBackAndCloses) {
  Fixture fixture;
  CHECK(fixture.start(/*idle_ms=*/0, /*idle_in_tx_ms=*/100));

  CHECK(fixture.connection->execute("BEGIN").ok);
  CHECK(fixture.connection->execute("INSERT INTO t (id, v) VALUES (1, 10)").ok);

  // 事务开着不动：服务器应回滚并断开（否则快照会一直钉住旧版本）
  CHECK(fixture.wait_closed(3000));
  CHECK_EQ(fixture.server->metrics().idle_timeouts.load(), uint64_t{1});

  // 另开一条连接（直接建一个会话）确认插入被回滚了
  session::Session observer(fixture.store->connect());
  (void)observer.execute("USE shop");
  auto rows = observer.execute("SELECT id FROM t");
  CHECK(rows.has_value());
  if (rows.has_value()) {
    size_t count = 0;
    while (true) {
      auto row = (*rows)->next();
      if (!row.has_value()) {
        break;
      }
      ++count;
    }
    CHECK_EQ(count, size_t{0}); // 回滚了：一行都没有
  }
}

TEST(ServerLifecycle, GracefulShutdownNotifiesConnectionsAndStops) {
  Fixture fixture;
  CHECK(fixture.start(/*idle_ms=*/0, /*idle_in_tx_ms=*/0));
  CHECK(fixture.connection->execute("SELECT id FROM t").ok);
  CHECK(fixture.connection->execute("INSERT INTO t (id, v) VALUES (7, 70)").ok);

  const uint64_t statements = fixture.server->metrics().statements.load();
  CHECK(statements >= 2);

  // 优雅退出：连接被通知收尾（收到 BYE/EOF），server.run() 返回
  fixture.server->request_shutdown();
  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::milliseconds(3000);
  while (fixture.server->connection_count() > 0 &&
         std::chrono::steady_clock::now() < deadline) {
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
  }
  CHECK_EQ(fixture.server->connection_count(), size_t{0});
  CHECK(fixture.server->shutting_down());
  if (fixture.thread.joinable()) {
    fixture.thread.join(); // run() 应当已经返回
  }
  CHECK(fixture.thread.joinable() == false);
}

// 回归：对端在服务器开口**之前**就消失了。服务器写 HELLO 时对端已经没了，
// 没有任何 SIGPIPE 保护的话这一次 write() 会送 SIGPIPE，而默认处置是**杀掉
// 整个进程** —— 一个跑掉的客户端就能把服务器打死。断言：服务器活下来、正常收尾。
TEST(ServerLifecycle, PeerAlreadyGoneDoesNotKillTheServer) {
  Fixture fixture;
  CHECK(fixture.attach_with_peer_already_gone(/*idle_ms=*/0,
                                              /*idle_in_tx_ms=*/0));

  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::milliseconds(3000);
  while (fixture.server->connection_count() > 0 &&
         std::chrono::steady_clock::now() < deadline) {
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
  }
  CHECK_EQ(fixture.server->connection_count(), size_t{0});
  CHECK_EQ(fixture.server->metrics().connections_total.load(), uint64_t{1});
  CHECK(!fixture.server->shutting_down()); // 还在正常服务，不是被信号带走了
}
