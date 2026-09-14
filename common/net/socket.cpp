#include "common/net/socket.h"

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <utility>

#include "common/net/socket_util.h"

namespace common::net {
namespace {

std::string system_error(const char *operation) {
  return std::string(operation) + ": " + std::strerror(errno);
}

} // namespace

TcpSocket::TcpSocket(TcpSocket &&other) noexcept
    : fd_(std::exchange(other.fd_, -1)) {}

TcpSocket &TcpSocket::operator=(TcpSocket &&other) noexcept {
  if (this != &other) {
    close();
    fd_ = std::exchange(other.fd_, -1);
  }
  return *this;
}

std::expected<TcpSocket, std::string>
TcpSocket::connect(std::string_view host, uint16_t port) {
  TcpSocket socket(::socket(AF_INET, SOCK_STREAM, 0));
  if (!socket.valid()) {
    return std::unexpected(system_error("socket()"));
  }
  socket_suppress_sigpipe(socket.fd());

  const std::string host_string(host);
  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_port = htons(port);
  if (::inet_pton(AF_INET, host_string.c_str(), &addr.sin_addr) != 1) {
    return std::unexpected("host must be an IPv4 address: " + host_string);
  }
  if (::connect(socket.fd(), reinterpret_cast<sockaddr *>(&addr),
                sizeof(addr)) != 0) {
    return std::unexpected("connect " + host_string + ":" +
                           std::to_string(port) +
                           " failed: " + std::strerror(errno));
  }
  return socket;
}

std::expected<TcpSocket, std::string>
TcpSocket::listen(std::string_view host, uint16_t port, int backlog,
                  uint16_t *bound_port) {
  TcpSocket socket(::socket(AF_INET, SOCK_STREAM, 0));
  if (!socket.valid()) {
    return std::unexpected(system_error("socket()"));
  }
  int reuse = 1;
  ::setsockopt(socket.fd(), SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

  const std::string host_string(host);
  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_port = htons(port);
  if (host_string.empty() || host_string == "*" ||
      host_string == "0.0.0.0") {
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
  } else if (::inet_pton(AF_INET, host_string.c_str(), &addr.sin_addr) != 1) {
    return std::unexpected("listen host must be an IPv4 address: " +
                           host_string);
  }
  if (::bind(socket.fd(), reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) !=
      0) {
    return std::unexpected(system_error("bind()"));
  }
  if (::listen(socket.fd(), backlog) != 0) {
    return std::unexpected(system_error("listen()"));
  }
  if (auto ok = socket.set_nonblocking(); !ok.has_value()) {
    return std::unexpected(ok.error());
  }

  if (bound_port != nullptr) {
    sockaddr_in bound{};
    socklen_t bound_len = sizeof(bound);
    if (::getsockname(socket.fd(), reinterpret_cast<sockaddr *>(&bound),
                      &bound_len) == 0) {
      *bound_port = ntohs(bound.sin_port);
    } else {
      *bound_port = port;
    }
  }
  return socket;
}

int TcpSocket::release() { return std::exchange(fd_, -1); }

void TcpSocket::close() {
  if (fd_ >= 0) {
    ::close(fd_);
    fd_ = -1;
  }
}

std::expected<void, std::string> TcpSocket::set_nonblocking() const {
  const int flags = ::fcntl(fd_, F_GETFL, 0);
  if (flags < 0) {
    return std::unexpected(system_error("fcntl(F_GETFL)"));
  }
  if (::fcntl(fd_, F_SETFL, flags | O_NONBLOCK) < 0) {
    return std::unexpected(system_error("fcntl(O_NONBLOCK)"));
  }
  return {};
}

std::expected<void, std::string> TcpSocket::set_tcp_nodelay() const {
  int enabled = 1;
  if (::setsockopt(fd_, IPPROTO_TCP, TCP_NODELAY, &enabled,
                   sizeof(enabled)) != 0) {
    return std::unexpected(system_error("setsockopt(TCP_NODELAY)"));
  }
  return {};
}

bool TcpSocket::send_all(std::string_view data, std::string *error) const {
  size_t sent = 0;
  while (sent < data.size()) {
    const ssize_t wrote =
        socket_write(fd_, data.data() + sent, data.size() - sent);
    if (wrote > 0) {
      sent += static_cast<size_t>(wrote);
      continue;
    }
    if (wrote < 0 && errno == EINTR) {
      continue;
    }
    if (error != nullptr) {
      *error = system_error("send()");
    }
    return false;
  }
  return true;
}

ssize_t TcpSocket::read_once(char *buffer, size_t size) const {
  while (true) {
    const ssize_t got = ::read(fd_, buffer, size);
    if (got < 0 && errno == EINTR) {
      continue;
    }
    return got;
  }
}

} // namespace common::net
