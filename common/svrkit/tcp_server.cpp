#include "common/svrkit/tcp_server.h"

#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <exception>
#include <utility>

#include "common/net/socket_util.h"

namespace common::svrkit {
namespace {

void make_nonblocking(int fd) {
  const int flags = ::fcntl(fd, F_GETFL, 0);
  if (flags >= 0) {
    ::fcntl(fd, F_SETFL, flags | O_NONBLOCK);
  }
}

} // namespace

Loop &TcpConnection::loop() const { return owner_->loop(); }

void TcpConnection::shutdown_read() const {
  if (fd_ >= 0) {
    ::shutdown(fd_, SHUT_RD);
  }
}

Task TcpConnection::read_some(std::string &buffer, ssize_t &nread) const {
  while (true) {
    char chunk[8192];
    const ssize_t got = ::read(fd_, chunk, sizeof(chunk));
    if (got > 0) {
      buffer.append(chunk, static_cast<size_t>(got));
      nread = got;
      co_return;
    }
    if (got == 0) {
      nread = 0;
      co_return;
    }
    if (errno == EINTR) {
      continue;
    }
    if (errno == EAGAIN || errno == EWOULDBLOCK) {
      co_await WaitFd{fd_, POLLIN};
      continue;
    }
    nread = -1;
    co_return;
  }
}

Task TcpConnection::write_all(std::string data, bool &ok) const {
  size_t sent = 0;
  while (sent < data.size()) {
    // 对端跑掉时不能让 SIGPIPE 杀掉整个进程。
    const ssize_t wrote =
        net::socket_write(fd_, data.data() + sent, data.size() - sent);
    if (wrote > 0) {
      sent += static_cast<size_t>(wrote);
      continue;
    }
    if (wrote < 0 && errno == EINTR) {
      continue;
    }
    if (wrote < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
      co_await WaitFd{fd_, POLLOUT};
      continue;
    }
    ok = false;
    co_return;
  }
  ok = true;
}

TcpServer::TcpServer(TcpServerOptions options, ConnectionHandler handler)
    : options_(std::move(options)), handler_(std::move(handler)),
      loop_("tcp-server-io") {
  int fds[2] = {-1, -1};
  if (::pipe(fds) == 0) {
    signal_read_ = fds[0];
    signal_write_ = fds[1];
    make_nonblocking(signal_read_);
    make_nonblocking(signal_write_);
  }
}

TcpServer::~TcpServer() {
  stop();
  close_all_connections();
  if (signal_read_ >= 0) {
    ::close(signal_read_);
  }
  if (signal_write_ >= 0) {
    ::close(signal_write_);
  }
}

void TcpServer::log(const char *level, const std::string &message) const {
  if (options_.logger) {
    options_.logger(level, message);
  }
}

std::expected<void, std::string> TcpServer::listen(std::string_view host,
                                                   uint16_t port) {
  uint16_t bound_port = port;
  auto socket = net::TcpSocket::listen(host, port, options_.listen_backlog,
                                       &bound_port);
  if (!socket.has_value()) {
    return std::unexpected(socket.error());
  }
  listen_socket_ = std::move(*socket);
  port_ = bound_port;
  return {};
}

void TcpServer::request_shutdown() {
  if (signal_write_ < 0) {
    return;
  }
  const char byte = 's';
  const ssize_t ignored = ::write(signal_write_, &byte, 1);
  (void)ignored; // async-signal-safe
}

void TcpServer::stop() { loop_.stop(); }

void TcpServer::attach_connection(int fd) {
  if (fd < 0) {
    return;
  }
  net::TcpSocket socket = net::TcpSocket::adopt(fd);
  if (auto ok = socket.set_nonblocking(); !ok.has_value()) {
    log("error", "attach_connection: " + ok.error());
    return; // socket 析构时关闭 fd
  }
  net::socket_suppress_sigpipe(socket.fd());

  auto connection =
      std::shared_ptr<TcpConnection>(new TcpConnection(this, socket.release()));
  {
    std::lock_guard<std::mutex> lock(connections_mutex_);
    connections_list_.push_back(connection);
  }
  connections_.fetch_add(1);
  connections_total_.fetch_add(1);
  loop_.spawn(run_connection(std::move(connection)));
}

void TcpServer::run() {
  if (signal_read_ >= 0) {
    loop_.watch(signal_read_, POLLIN,
                [this](short) { begin_graceful_shutdown(); });
  }
  // 没有监听 fd（只 attach_connection 的测试/嵌入场景）时不要起 accept 协程：
  // WaitFd{fd<0} 会立刻 ready，while 循环会忙等并饿死事件循环。
  if (listen_socket_.valid()) {
    loop_.spawn(accept_loop());
  }
  loop_.run();

  // 硬停止路径：处理器协程可能没有机会收尾，至少把 fd 所有权收回来。
  close_all_connections();
  listen_socket_.close();
}

Task TcpServer::accept_loop() {
  while (!loop_.stopped()) {
    co_await WaitFd{listen_socket_.fd(), POLLIN};
    if (loop_.stopped()) {
      break;
    }
    while (true) {
      sockaddr_in peer{};
      socklen_t peer_len = sizeof(peer);
      const int fd = ::accept(listen_socket_.fd(),
                              reinterpret_cast<sockaddr *>(&peer), &peer_len);
      if (fd < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
          break;
        }
        if (errno == EINTR) {
          continue;
        }
        break;
      }
      if (connections_.load() >= options_.max_connections) {
        ::close(fd); // 超限：直接关，不做半开连接
        continue;
      }
      net::TcpSocket socket = net::TcpSocket::adopt(fd);
      (void)socket.set_tcp_nodelay();
      attach_connection(socket.release());
      log("debug", "connection accepted");
    }
  }
}

Task TcpServer::run_connection(
    std::shared_ptr<TcpConnection> connection) {
  try {
    co_await handler_(connection);
  } catch (const std::exception &error) {
    log("error", std::string("connection handler failed: ") + error.what());
  } catch (...) {
    log("error", "connection handler failed with an unknown exception");
  }
  close_connection(connection);
}

void TcpServer::close_connection(
    const std::shared_ptr<TcpConnection> &connection) {
  const int fd = connection->close_from_owner();
  if (fd >= 0) {
    ::close(fd);
  }

  {
    std::lock_guard<std::mutex> lock(connections_mutex_);
    for (auto it = connections_list_.begin(); it != connections_list_.end();
         ++it) {
      if (it->get() == connection.get()) {
        connections_list_.erase(it);
        break;
      }
    }
  }
  connections_.fetch_sub(1);
  log("debug", "connection closed");
}

void TcpServer::close_all_connections() {
  std::lock_guard<std::mutex> lock(connections_mutex_);
  for (auto &connection : connections_list_) {
    const int fd = connection->close_from_owner();
    if (fd >= 0) {
      ::close(fd);
    }
  }
  connections_list_.clear();
  connections_ = 0;
}

void TcpServer::begin_graceful_shutdown() {
  if (stopping_.exchange(true)) {
    return;
  }
  log("info", "shutdown: stop accepting new connections");
  if (listen_socket_.valid()) {
    loop_.unwatch(listen_socket_.fd());
    listen_socket_.close();
  }

  {
    std::lock_guard<std::mutex> lock(connections_mutex_);
    for (const auto &connection : connections_list_) {
      connection->mark_stopping();
      connection->shutdown_read();
    }
  }

  const int64_t deadline_ms = options_.shutdown_grace_ms;
  constexpr int64_t step_ms = 20;
  auto waited = std::make_shared<int64_t>(0);
  auto poll_connections = std::make_shared<std::function<void()>>();
  *poll_connections = [this, waited, poll_connections, deadline_ms] {
    if (connections_.load() == 0) {
      log("info", "shutdown: all connections closed");
      loop_.stop();
      return;
    }
    *waited += step_ms;
    if (*waited >= deadline_ms) {
      log("warn", "shutdown: connection(s) still open; stopping anyway");
      loop_.stop();
      return;
    }
    loop_.add_timer(step_ms, *poll_connections);
  };
  loop_.add_timer(step_ms, *poll_connections);
}

} // namespace common::svrkit
