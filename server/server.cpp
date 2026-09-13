// server/server.cpp
#include "server.h"

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cstring>
#include <utility>

#include "protocol.h"

namespace server {
namespace {

constexpr uint16_t kServerVersion = 1;

int set_nonblocking(int fd) {
  const int flags = ::fcntl(fd, F_GETFL, 0);
  if (flags < 0) {
    return -1;
  }
  return ::fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

// 在协程里读一次（可读事件驱动）；返回：>0 读到的字节，0 = 对端关闭，
// -1 = 真错误，-2 = 这次没数据（继续等）
Task read_some(int fd, std::string &buffer, ssize_t &nread) {
  while (true) {
    char chunk[8192];
    const ssize_t got = ::read(fd, chunk, sizeof(chunk));
    if (got > 0) {
      buffer.append(chunk, static_cast<size_t>(got));
      nread = got;
      co_return;
    }
    if (got == 0) {
      nread = 0; // 对端关闭
      co_return;
    }
    if (errno == EINTR) {
      continue;
    }
    if (errno == EAGAIN || errno == EWOULDBLOCK) {
      co_await WaitFd{fd, POLLIN};
      continue;
    }
    nread = -1;
    co_return;
  }
}

// 在协程里把 data 写完
Task write_all(int fd, std::string data, bool &ok) {
  size_t sent = 0;
  while (sent < data.size()) {
    const ssize_t wrote = ::write(fd, data.data() + sent, data.size() - sent);
    if (wrote > 0) {
      sent += static_cast<size_t>(wrote);
      continue;
    }
    if (wrote < 0 && errno == EINTR) {
      continue;
    }
    if (wrote < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
      co_await WaitFd{fd, POLLOUT};
      continue;
    }
    ok = false;
    co_return;
  }
  ok = true;
}

ErrorFrame to_error_frame(const session::SessionError &error) {
  ErrorFrame frame;
  frame.code = static_cast<uint8_t>(error.code);
  frame.message = error.to_string();
  frame.sql = error.sql;
  frame.begin_line = error.span.begin_line;
  frame.begin_column = error.span.begin_column;
  frame.end_line = error.span.end_line;
  frame.end_column = error.span.end_column;
  return frame;
}

} // namespace

Server::Server(Config config, std::shared_ptr<kv::KVStore> store)
    : config_(std::move(config)), store_(std::move(store)), loop_("server-io"),
      parse_service_("parse-service"), write_service_("write-service") {}

Server::~Server() {
  parse_service_.stop();
  write_service_.stop();
  if (listen_fd_ >= 0) {
    ::close(listen_fd_);
  }
}

std::expected<void, std::string> Server::listen() {
  if (store_ == nullptr || !store_->is_open()) {
    return std::unexpected("storage is not open");
  }
  listen_fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
  if (listen_fd_ < 0) {
    return std::unexpected(std::string("socket(): ") + std::strerror(errno));
  }
  int reuse = 1;
  ::setsockopt(listen_fd_, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

  // 端口 0 = 让内核挑（测试用）
  const int requested_port = std::stoi(config_.port);
  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_port = htons(static_cast<uint16_t>(requested_port));
  if (config_.host.empty() || config_.host == "*" ||
      config_.host == "0.0.0.0") {
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
  } else if (::inet_pton(AF_INET, config_.host.c_str(), &addr.sin_addr) != 1) {
    return std::unexpected("listen host must be an IPv4 address: " +
                           config_.host);
  }
  if (::bind(listen_fd_, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) !=
      0) {
    return std::unexpected(std::string("bind(): ") + std::strerror(errno));
  }
  if (::listen(listen_fd_, 128) != 0) {
    return std::unexpected(std::string("listen(): ") + std::strerror(errno));
  }
  if (set_nonblocking(listen_fd_) != 0) {
    return std::unexpected(std::string("fcntl(O_NONBLOCK): ") +
                           std::strerror(errno));
  }
  sockaddr_in bound{};
  socklen_t bound_len = sizeof(bound);
  if (::getsockname(listen_fd_, reinterpret_cast<sockaddr *>(&bound),
                    &bound_len) == 0) {
    port_ = ntohs(bound.sin_port); // port = 0 时这里拿到真实端口
  } else {
    port_ = requested_port;
  }
  return {};
}

void Server::run() {
  parse_service_.start(config_.write_queue_max);
  write_service_.start(config_.write_queue_max);
  loop_.spawn(accept_loop());
  loop_.run();
}

Task Server::accept_loop() {
  while (!loop_.stopped()) {
    co_await WaitFd{listen_fd_, POLLIN};
    if (loop_.stopped()) {
      break;
    }
    while (true) {
      sockaddr_in peer{};
      socklen_t peer_len = sizeof(peer);
      const int fd =
          ::accept(listen_fd_, reinterpret_cast<sockaddr *>(&peer), &peer_len);
      if (fd < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
          break; // 这一轮 accept 完了，回去等可读
        }
        if (errno == EINTR) {
          continue;
        }
        break;
      }
      if (connections_.load() >= config_.max_connections) {
        ::close(fd); // 超限：直接关，不做半开连接
        continue;
      }
      if (set_nonblocking(fd) != 0) {
        ::close(fd);
        continue;
      }
      int nodelay = 1;
      ::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &nodelay, sizeof(nodelay));
      connections_.fetch_add(1);
      loop_.spawn(serve_connection(fd));
    }
  }
}

Task Server::serve_connection(int fd) {
  session::Session session(store_->connect());
  if (!config_.default_database.empty()) {
    (void)session.execute("USE " + config_.default_database);
  }

  std::string in;
  bool alive = true;
  // 1) HELLO（协议版本 + 能力位）
  {
    bool ok = false;
    co_await write_all(fd, encode_hello(kServerVersion, 0), ok);
    alive = ok;
  }

  const size_t read_threads = config_.read_threads; // M1：读池 = 本 Loop
  (void)read_threads;

  while (alive && !loop_.stopped()) {
    // 2) 收帧（半帧/粘包都在这里处理）
    DecodedFrame frame;
    size_t consumed = 0;
    std::string error;
    while (!try_decode_frame(in, &frame, &consumed, &error)) {
      if (!error.empty()) {
        alive = false; // 协议错：断开
        break;
      }
      ssize_t nread = 0;
      co_await read_some(fd, in, nread);
      if (nread <= 0) {
        alive = false;
        break;
      }
    }
    if (!alive) {
      break;
    }
    in.erase(0, consumed);

    if (frame.type == FrameType::kBye) {
      break;
    }
    if (frame.type == FrameType::kPing) {
      bool ok = false;
      co_await write_all(fd, encode_simple(FrameType::kPing), ok);
      alive = ok;
      continue;
    }
    if (frame.type != FrameType::kQuery) {
      bool ok = false;
      ErrorFrame bad;
      bad.message = "unexpected frame from client";
      co_await write_all(fd, encode_error(bad), ok);
      alive = ok;
      continue;
    }

    // 3) QUERY：解析走 parse 服务线程，执行按路由规则走写服务或就地
    std::string sql;
    if (!decode_query(frame.payload, &sql)) {
      alive = false;
      break;
    }

    // 前置检查（与 session::execute 一致：空语句/存储未打开先报，不看语法）
    std::expected<session::ParsedStatement, session::SessionError> parsed =
        std::unexpected(session::SessionError());
    {
      SubmitToService submit;
      submit.service = &parse_service_;
      submit.work = [&session, &sql, &parsed] { parsed = session.parse(sql); };
      co_await submit;
      if (!submit.submitted) {
        bool ok = false;
        ErrorFrame busy;
        busy.message = "server busy: parse queue is full";
        co_await write_all(fd, encode_error(busy), ok);
        alive = ok;
        continue;
      }
    }

    session::SessionError parse_error;
    if (!parsed.has_value()) {
      parse_error = session.parse_error_to_session_error(parsed.error());
    }

    std::expected<std::unique_ptr<exec::ResultCursor>, session::SessionError>
        result = std::unexpected(session::SessionError());
    if (parsed.has_value()) {
      // 路由：事务里的语句 + 非只读语句 → 写服务线程；只读且不在事务 → 就地执行
      const bool route_to_write =
          session.in_transaction() || !parsed->read_only;
      if (route_to_write) {
        SubmitToService submit;
        submit.service = &write_service_;
        submit.work = [&session, &parsed, &result] {
          result = session.execute_parsed(*parsed);
        };
        co_await submit;
        if (!submit.submitted) {
          bool ok = false;
          ErrorFrame busy;
          busy.message = "server busy: write queue is full";
          co_await write_all(fd, encode_error(busy), ok);
          alive = ok;
          continue;
        }
      } else {
        result = session.execute_parsed(*parsed);
      }
    }

    // 4) 回结果：ERROR / COLUMNS+ROW* / OK
    std::string out;
    if (!parsed.has_value()) {
      out = encode_error(to_error_frame(parse_error));
    } else if (!result.has_value()) {
      out = encode_error(to_error_frame(result.error()));
    } else {
      exec::ResultCursor &cursor = **result;
      if (cursor.root()->produces_rows()) {
        out += encode_columns(cursor.columns());
        size_t rows = 0;
        bool truncated = false;
        while (true) {
          auto row = cursor.next();
          if (!row.has_value()) {
            if (row.error().is_error()) {
              out = encode_error(
                  ErrorFrame{0, row.error().to_string(), "", 0, 0, 0, 0});
              break;
            }
            break;
          }
          if (++rows > config_.max_result_rows) {
            truncated = true;
            break;
          }
          std::vector<ProtocolValue> values;
          values.reserve(row->size());
          for (size_t i = 0; i < row->size(); ++i) {
            ProtocolValue value;
            value.is_null = (*row)[i].is_null();
            value.text = value.is_null ? std::string() : (*row)[i].to_string();
            values.push_back(std::move(value));
          }
          out += encode_row(values);
        }
        if (truncated) {
          ErrorFrame too_many;
          too_many.message = "result set too large (max_result_rows=" +
                             std::to_string(config_.max_result_rows) + ")";
          out = encode_error(too_many);
        } else if (!cursor.error().is_error()) {
          out += encode_ok(static_cast<uint64_t>(rows));
        }
      } else if (cursor.error().is_error()) {
        out = encode_error(
            ErrorFrame{0, cursor.error().to_string(), "", 0, 0, 0, 0});
      } else {
        out = encode_ok(static_cast<uint64_t>(cursor.affected_rows()));
      }
    }

    bool ok = false;
    co_await write_all(fd, std::move(out), ok);
    alive = ok;
  }

  bool ignored = false;
  co_await write_all(fd, encode_simple(FrameType::kBye), ignored);
  ::close(fd);
  connections_.fetch_sub(1);
}

} // namespace server
