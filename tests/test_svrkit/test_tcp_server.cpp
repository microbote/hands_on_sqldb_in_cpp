// tests/test_svrkit/test_tcp_server.cpp
//
// 通用 TCP 服务器框架自身的行为：应用层只给 handler，框架管连接生命周期。
// 全部用 socketpair，不依赖 bind()（受限沙箱也能跑）。
#include "test_framework.h"

#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <functional>
#include <memory>
#include <string>
#include <thread>

#include "common/net/socket.h"
#include "common/svrkit/tcp_server.h"

namespace {

struct SocketPair {
  SocketPair() {
    if (::socketpair(AF_UNIX, SOCK_STREAM, 0, fds) != 0) {
      fds[0] = fds[1] = -1;
    }
  }
  ~SocketPair() {
    if (fds[0] >= 0) {
      ::close(fds[0]);
    }
    if (fds[1] >= 0) {
      ::close(fds[1]);
    }
  }

  int take_left() {
    const int fd = fds[0];
    fds[0] = -1;
    return fd;
  }

  int fds[2] = {-1, -1};
};

bool wait_readable(int fd, int timeout_ms) {
  pollfd entry{};
  entry.fd = fd;
  entry.events = POLLIN;
  return ::poll(&entry, 1, timeout_ms) > 0;
}

bool wait_until(const std::function<bool()> &done, int timeout_ms) {
  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
  while (std::chrono::steady_clock::now() < deadline) {
    if (done()) {
      return true;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  return done();
}

} // namespace

TEST(TcpSocket, AdoptedSocketPairSendsBothWays) {
  SocketPair pair;
  CHECK(pair.fds[0] >= 0);
  CHECK(pair.fds[1] >= 0);

  auto left = common::net::TcpSocket::adopt(pair.take_left());
  auto right = common::net::TcpSocket::adopt(pair.fds[1]);
  pair.fds[1] = -1;

  CHECK(left.send_all("ping"));
  char buffer[16] = {};
  CHECK(wait_readable(right.fd(), 1000));
  CHECK_EQ(right.read_once(buffer, sizeof(buffer)), ssize_t{4});
  CHECK_EQ(std::string(buffer, 4), std::string("ping"));

  CHECK(right.send_all("pong"));
  std::string response;
  char chunk[16] = {};
  CHECK(wait_readable(left.fd(), 1000));
  const ssize_t got = left.read_once(chunk, sizeof(chunk));
  CHECK(got > 0);
  if (got > 0) {
    response.assign(chunk, static_cast<size_t>(got));
  }
  CHECK_EQ(response, std::string("pong"));
}

TEST(TcpServer, AttachedConnectionRunsHandlerAndFrameworkClosesIt) {
  SocketPair pair;
  CHECK(pair.fds[0] >= 0);
  CHECK(pair.fds[1] >= 0);

  std::atomic<bool> wrote_response{false};
  common::svrkit::TcpServer server(
      common::svrkit::TcpServerOptions{},
      [&](std::shared_ptr<common::svrkit::TcpConnection> connection)
          -> common::svrkit::Task {
        std::string input;
        ssize_t nread = -1;
        co_await connection->read_some(input, nread);
        if (nread > 0) {
          bool ok = false;
          co_await connection->write_all("echo:" + input, ok);
          wrote_response = ok;
        }
      });

  server.attach_connection(pair.take_left());
  std::thread runner([&] { server.run(); });

  CHECK_EQ(::write(pair.fds[1], "ping", 4), ssize_t{4});
  CHECK(wait_readable(pair.fds[1], 2000));
  char buffer[32] = {};
  const ssize_t got = ::read(pair.fds[1], buffer, sizeof(buffer));
  CHECK(got > 0);
  if (got > 0) {
    CHECK_EQ(std::string(buffer, static_cast<size_t>(got)),
             std::string("echo:ping"));
  }
  CHECK(wrote_response.load());
  CHECK(wait_until([&] { return server.connection_count() == 0; }, 2000));

  server.stop();
  runner.join();
  CHECK_EQ(server.connections_total(), uint64_t{1});
}

TEST(TcpServer, GracefulShutdownWakesHandlerAndStopsLoop) {
  SocketPair pair;
  CHECK(pair.fds[0] >= 0);
  CHECK(pair.fds[1] >= 0);

  std::atomic<bool> stopping_seen{false};
  std::atomic<bool> bye_written{false};
  common::svrkit::TcpServer server(
      common::svrkit::TcpServerOptions{},
      [&](std::shared_ptr<common::svrkit::TcpConnection> connection)
          -> common::svrkit::Task {
        std::string input;
        ssize_t nread = -1;
        co_await connection->read_some(input, nread);
        stopping_seen = connection->stopping();
        bool ok = false;
        co_await connection->write_all("bye", ok);
        bye_written = ok;
      });

  server.attach_connection(pair.take_left());
  std::thread runner([&] { server.run(); });
  CHECK(wait_until([&] { return server.connection_count() == 1; }, 2000));

  server.request_shutdown();
  CHECK(wait_readable(pair.fds[1], 2000));
  char buffer[8] = {};
  const ssize_t got = ::read(pair.fds[1], buffer, sizeof(buffer));
  CHECK(got > 0);
  if (got > 0) {
    CHECK_EQ(std::string(buffer, static_cast<size_t>(got)),
             std::string("bye"));
  }

  CHECK(wait_until([&] { return server.connection_count() == 0; }, 2000));
  CHECK(wait_until([&] { return server.loop().stopped(); }, 2000));
  runner.join();
  CHECK(stopping_seen.load());
  CHECK(bye_written.load());
  CHECK(server.shutting_down());
}

TEST(TcpServer, DestructorClosesAttachedConnectionEvenBeforeRun) {
  SocketPair pair;
  CHECK(pair.fds[0] >= 0);
  CHECK(pair.fds[1] >= 0);

  {
    common::svrkit::TcpServer server(
        common::svrkit::TcpServerOptions{},
        [](std::shared_ptr<common::svrkit::TcpConnection>)
            -> common::svrkit::Task { co_return; });
    server.attach_connection(pair.take_left());
    CHECK_EQ(server.connection_count(), size_t{1});
  }

  CHECK(wait_readable(pair.fds[1], 1000));
  char byte = 0;
  CHECK_EQ(::read(pair.fds[1], &byte, 1), ssize_t{0}); // EOF：对端已被析构关闭
}
