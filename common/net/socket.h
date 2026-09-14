// common/net/socket.h
//
// TCP socket 的通用封装：给 svrkit（异步服务器）和阻塞式客户端共用。
// 这里只处理"连接/监听/读写一次/所有权"，不关心上层协议。
#pragma once

#include <cstddef>
#include <cstdint>
#include <expected>
#include <string>
#include <string_view>
#include <sys/types.h>

namespace common::net {

class TcpSocket {
public:
  TcpSocket() = default;
  explicit TcpSocket(int fd) : fd_(fd) {}
  ~TcpSocket() { close(); }

  TcpSocket(const TcpSocket &) = delete;
  TcpSocket &operator=(const TcpSocket &) = delete;
  TcpSocket(TcpSocket &&other) noexcept;
  TcpSocket &operator=(TcpSocket &&other) noexcept;

  // 阻塞式 TCP 客户端建连（host 目前只收 IPv4 字面量）。
  static std::expected<TcpSocket, std::string>
  connect(std::string_view host, uint16_t port);

  // TCP 监听：port=0 时通过 bound_port 返回内核分配的真实端口。
  static std::expected<TcpSocket, std::string>
  listen(std::string_view host, uint16_t port, int backlog,
         uint16_t *bound_port = nullptr);

  // 接管一个已经建立的 fd（例如 socketpair 的一半）。
  static TcpSocket adopt(int fd) { return TcpSocket(fd); }

  bool valid() const { return fd_ >= 0; }
  int fd() const { return fd_; }
  int release();
  void close();

  std::expected<void, std::string> set_nonblocking() const;
  std::expected<void, std::string> set_tcp_nodelay() const;

  // 阻塞写完整段数据（EINTR 自动重试）；失败时填 error。
  bool send_all(std::string_view data, std::string *error = nullptr) const;
  // 阻塞读一次（EINTR 自动重试）；返回值与 read(2) 一致。
  ssize_t read_once(char *buffer, size_t size) const;

private:
  int fd_ = -1;
};

} // namespace common::net
