// common/svrkit/tcp_server.h
//
// 协议无关的 TCP 服务器框架：Raft/SQL 这类应用只需要提供一个
// ConnectionHandler，框架负责 listen/accept、连接所有权、非阻塞 I/O、
// 连接上限、跨线程停止与优雅退出。
//
// 连接处理器是协程：
//   svrkit::TcpServer server(options, [](auto conn) -> svrkit::Task {
//     co_await conn->write_all(...);
//     co_await conn->read_some(...);
//   });
//
// 处理器返回后框架统一关闭 fd 并注销连接；应用层如果还有 BYE 这类协议
// 收尾帧，应在返回前发送。
#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <sys/types.h>
#include <utility>
#include <vector>

#include "common/net/socket.h"
#include "common/svrkit/loop.h"
#include "common/svrkit/task.h"

namespace common::svrkit {

class TcpServer;

class TcpConnection {
public:
  int fd() const { return fd_; }
  Loop &loop() const;
  bool stopping() const { return stopping_.load(); }

  // 让阻塞在读事件上的连接协程醒来（仍保留写方向，方便回错误/收尾帧）。
  void shutdown_read() const;

  // 协程式读写：读返回 >0 字节、0 对端关闭、-1 错误；写返回 ok。
  Task read_some(std::string &buffer, ssize_t &nread) const;
  Task write_all(std::string data, bool &ok) const;

private:
  friend class TcpServer;
  TcpConnection(TcpServer *owner, int fd) : owner_(owner), fd_(fd) {}

  void mark_stopping() { stopping_ = true; }
  int close_from_owner() { return std::exchange(fd_, -1); }

  TcpServer *owner_ = nullptr;
  int fd_ = -1;
  std::atomic<bool> stopping_{false};
};

struct TcpServerOptions {
  size_t max_connections = 128;
  int listen_backlog = 128;
  int64_t shutdown_grace_ms = 5000;
  // level 使用 error/warn/info/debug；为空则静默。
  std::function<void(const char *, const std::string &)> logger;
};

class TcpServer {
public:
  using ConnectionHandler =
      std::function<Task(std::shared_ptr<TcpConnection>)>;

  TcpServer(TcpServerOptions options, ConnectionHandler handler);
  ~TcpServer();

  TcpServer(const TcpServer &) = delete;
  TcpServer &operator=(const TcpServer &) = delete;

  std::expected<void, std::string> listen(std::string_view host,
                                          uint16_t port);
  void run();
  // 立即停事件循环（测试/异常路径用；不等连接协议收尾）。
  void stop();
  // 可从信号处理函数调用（内部只写 self-pipe）。
  void request_shutdown();

  // 挂入一条已建立的连接（socketpair 测试、外部 accept 后转交）。
  // 成功后 fd 归 TcpServer 所有；失败时框架会关闭它。
  void attach_connection(int fd);

  Loop &loop() { return loop_; }
  int port() const { return port_; }
  size_t connection_count() const { return connections_.load(); }
  uint64_t connections_total() const { return connections_total_.load(); }
  bool shutting_down() const { return stopping_.load(); }

private:
  Task accept_loop();
  Task run_connection(std::shared_ptr<TcpConnection> connection);
  void begin_graceful_shutdown();
  void close_connection(const std::shared_ptr<TcpConnection> &connection);
  void close_all_connections();
  void log(const char *level, const std::string &message) const;

  TcpServerOptions options_;
  ConnectionHandler handler_;
  Loop loop_;
  net::TcpSocket listen_socket_;
  int port_ = 0;

  std::atomic<size_t> connections_{0};
  std::atomic<uint64_t> connections_total_{0};
  std::atomic<bool> stopping_{false};
  int signal_read_ = -1;
  int signal_write_ = -1;

  std::mutex connections_mutex_;
  std::vector<std::shared_ptr<TcpConnection>> connections_list_;
};

} // namespace common::svrkit
