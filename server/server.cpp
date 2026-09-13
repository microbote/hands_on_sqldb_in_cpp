// server/server.cpp
#include "server.h"

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <chrono>
#include <cstring>
#include <functional>
#include <utility>

#include <fmt/format.h>

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
      parse_service_("parse-service"), write_service_("write-service") {
  int fds[2] = {-1, -1};
  if (::pipe(fds) == 0) {
    signal_read_ = fds[0];
    signal_write_ = fds[1];
    const auto set_nonblock = [](int fd) {
      const int flags = ::fcntl(fd, F_GETFL, 0);
      if (flags >= 0) {
        ::fcntl(fd, F_SETFL, flags | O_NONBLOCK);
      }
    };
    set_nonblock(signal_read_);
    set_nonblock(signal_write_);
  }
}

Server::~Server() {
  parse_service_.stop();
  write_service_.stop();
  if (listen_fd_ >= 0) {
    ::close(listen_fd_);
  }
  if (signal_read_ >= 0) {
    ::close(signal_read_);
  }
  if (signal_write_ >= 0) {
    ::close(signal_write_);
  }
}

void Server::log(const char *level, const std::string &message) const {
  const auto rank = [](const std::string &name) {
    if (name == "error") {
      return 0;
    }
    if (name == "warn") {
      return 1;
    }
    return name == "info" ? 2 : 3;
  };
  if (rank(level) > rank(config_.log_level)) {
    return;
  }
  fmt::print(stderr, "[{}] {}\n", level, message);
}

void Server::request_shutdown() {
  if (signal_write_ < 0) {
    return;
  }
  const char byte = 's';
  ssize_t ignored = ::write(signal_write_, &byte, 1); // async-signal-safe
  (void)ignored;
}

void Server::register_connection(const std::shared_ptr<ConnState> &state) {
  std::lock_guard<std::mutex> lock(connections_mutex_);
  connections_list_.push_back(state);
}

void Server::unregister_connection(int fd) {
  std::lock_guard<std::mutex> lock(connections_mutex_);
  for (auto it = connections_list_.begin(); it != connections_list_.end();
       ++it) {
    if ((*it)->fd == fd) {
      connections_list_.erase(it);
      return;
    }
  }
}

void Server::arm_idle_watchdog(const std::shared_ptr<ConnState> &state,
                               bool in_tx) {
  const int64_t timeout_ms =
      in_tx ? config_.idle_in_transaction_timeout_ms : config_.idle_timeout_ms;
  if (timeout_ms <= 0) {
    return; // 0 = 不超时
  }
  const uint64_t generation = state->generation.fetch_add(1) + 1;
  const int fd = state->fd;
  loop_.add_timer(timeout_ms, [this, state, fd, generation] {
    if (state->generation.load() != generation || state->stopping.load()) {
      return; // 连接已经往前走了（读到新语句）或正在收尾
    }
    state->timed_out = true;
    metrics_.idle_timeouts.fetch_add(1);
    // 只关**读方向**：协程会从 read 里醒来，还能把 ERROR 帧写回去
    ::shutdown(fd, SHUT_RD);
  });
}

void Server::begin_graceful_shutdown() {
  if (stopping_.exchange(true)) {
    return;
  }
  log("info", "shutdown: stop accepting new connections");
  loop_.unwatch(listen_fd_);

  {
    std::lock_guard<std::mutex> lock(connections_mutex_);
    for (const auto &state : connections_list_) {
      state->stopping = true;
      state->generation.fetch_add(1); // 让空闲看门狗失效
      ::shutdown(state->fd, SHUT_RD);
    }
  }

  // 等连接退出（上限 5s，之后强制停 loop）
  const int64_t deadline_ms = 5000;
  const int64_t step_ms = 20;
  auto waited = std::make_shared<int64_t>(0);
  auto poll_connections = std::make_shared<std::function<void()>>();
  *poll_connections = [this, waited, poll_connections, deadline_ms]() {
    const int64_t step_ms = 20;
    if (connections_.load() == 0) {
      log("info", "shutdown: all connections closed");
      loop_.stop();
      return;
    }
    *waited += step_ms;
    if (*waited >= deadline_ms) {
      log("warn",
          fmt::format("shutdown: {} connection(s) still open after {}ms, "
                      "stopping anyway",
                      connections_.load(), deadline_ms));
      loop_.stop();
      return;
    }
    loop_.add_timer(step_ms, *poll_connections);
  };
  loop_.add_timer(step_ms, *poll_connections);
}

void Server::attach_connection(int fd) {
  // 和 accept 路径一样：socket 必须非阻塞 —— 否则一次 read 就把整条
  // 事件循环线程堵死（定时器、别的连接全都不转了）
  if (set_nonblocking(fd) != 0) {
    log("error", fmt::format("attach_connection: fcntl(O_NONBLOCK) failed: {}",
                             std::strerror(errno)));
    ::close(fd);
    return;
  }
  auto state = std::make_shared<ConnState>();
  state->fd = fd;
  register_connection(state);
  connections_.fetch_add(1);
  metrics_.connections_total.fetch_add(1);
  loop_.spawn(serve_connection(fd, state));
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
  if (signal_read_ >= 0) {
    loop_.watch(signal_read_, POLLIN,
                [this](short) { begin_graceful_shutdown(); });
  }
  loop_.spawn(accept_loop());
  loop_.run();
  log("info",
      fmt::format("stopped: connections={} statements={} errors={} rows={} "
                  "idle_timeouts={} write_rejects={} parse_rejects={}",
                  metrics_.connections_total.load(), metrics_.statements.load(),
                  metrics_.errors.load(), metrics_.rows_sent.load(),
                  metrics_.idle_timeouts.load(),
                  metrics_.write_queue_rejected.load(),
                  metrics_.parse_queue_rejected.load()));
  parse_service_.stop();
  write_service_.stop();
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
      auto state = std::make_shared<ConnState>();
      state->fd = fd;
      register_connection(state);
      connections_.fetch_add(1);
      metrics_.connections_total.fetch_add(1);
      log("debug", fmt::format("connection accepted: fd={}", fd));
      loop_.spawn(serve_connection(fd, state));
    }
  }
}

Task Server::serve_connection(int fd, std::shared_ptr<ConnState> state) {
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
      // 等下一帧之前装上空闲看门狗：
      //   - 不在事务里 -> idle_timeout_ms；
      //   - 在事务里     -> idle_in_transaction_timeout_ms（防快照钉住旧版本）
      arm_idle_watchdog(state, session.in_transaction());
      ssize_t nread = 0;
      co_await read_some(fd, in, nread);
      if (nread <= 0) {
        if (state->timed_out.load()) {
          // 超时：尽力回一个 ERROR 帧告诉对端原因（读方向已经关了，写还能用）
          const bool was_in_tx = session.in_transaction();
          if (was_in_tx) {
            (void)session.execute("ROLLBACK"); // 释放快照/写槽
          }
          ErrorFrame timeout;
          timeout.message =
              was_in_tx
                  ? "connection terminated: idle in transaction for too long "
                    "(transaction rolled back)"
                  : "connection terminated: idle for too long";
          bool ignored = false;
          co_await write_all(fd, encode_error(timeout), ignored);
          log("info", fmt::format("connection fd={} closed: idle timeout{}", fd,
                                  was_in_tx ? " (in transaction)" : ""));
        }
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
    if (frame.type == FrameType::kMeta) {
      // 元信息（\l / \dt / \d）：读 Catalog，就地执行（不占写槽）
      uint8_t kind = 0;
      std::string arg1;
      std::string arg2;
      std::string payload;
      bool replied = false;
      bool ok = false;
      if (!decode_meta(frame.payload, &kind, &arg1, &arg2)) {
        alive = false;
        break;
      }
      switch (static_cast<MetaKind>(kind)) {
      case MetaKind::kDatabases: {
        std::vector<MetaDatabase> databases;
        for (const auto &info : session.databases()) {
          MetaDatabase db;
          db.name = info.name.str();
          db.created_at = info.created_at;
          db.table_count = static_cast<uint32_t>(info.table_count);
          db.is_current = info.is_current;
          databases.push_back(std::move(db));
        }
        payload = encode_meta_databases(databases);
        break;
      }
      case MetaKind::kTables: {
        std::vector<MetaTable> tables;
        for (const auto &info : session.tables(sql::Identifier(arg1))) {
          MetaTable table;
          table.name = info.name.str();
          table.column_count = static_cast<uint32_t>(info.column_count);
          table.primary_key = info.primary_key.str();
          table.created_at = info.created_at;
          table.last_write_at = info.last_write_at;
          table.row_count = static_cast<uint64_t>(info.row_count);
          tables.push_back(std::move(table));
        }
        payload = encode_meta_tables(tables);
        break;
      }
      case MetaKind::kSchema: {
        auto schema =
            session.table_schema(sql::Identifier(arg2), sql::Identifier(arg1));
        if (!schema.has_value()) {
          ErrorFrame missing;
          missing.message = "table not found: " + arg2;
          co_await write_all(fd, encode_error(missing), ok);
          alive = ok;
          replied = true;
          break;
        }
        payload = encode_meta_schema(*schema);
        break;
      }
      default:
        alive = false;
        break;
      }
      if (!alive) {
        break;
      }
      if (replied) {
        continue; // 已经回过错误帧了
      }
      co_await write_all(fd, encode_meta_reply(payload), ok);
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
    metrics_.statements.fetch_add(1);

    // 前置检查（与 session::execute 一致：空语句/存储未打开先报，不看语法）
    std::expected<session::ParsedStatement, session::SessionError> parsed =
        std::unexpected(session::SessionError());
    {
      SubmitToService submit;
      submit.service = &parse_service_;
      submit.work = [&session, &sql, &parsed] { parsed = session.parse(sql); };
      co_await submit;
      if (!submit.submitted) {
        metrics_.parse_queue_rejected.fetch_add(1);
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
          metrics_.write_queue_rejected.fetch_add(1);
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
    bool error_response = false;
    uint64_t rows_sent = 0;
    if (!parsed.has_value()) {
      error_response = true;
      out = encode_error(to_error_frame(parse_error));
    } else if (!result.has_value()) {
      error_response = true;
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
              error_response = true;
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
          error_response = true;
          ErrorFrame too_many;
          too_many.message = "result set too large (max_result_rows=" +
                             std::to_string(config_.max_result_rows) + ")";
          out = encode_error(too_many);
        } else if (!cursor.error().is_error()) {
          rows_sent = static_cast<uint64_t>(rows);
          out +=
              encode_ok(static_cast<uint64_t>(rows), session.in_transaction(),
                        /*is_write=*/false, session.current_database().str());
        }
      } else if (cursor.error().is_error()) {
        error_response = true;
        out = encode_error(
            ErrorFrame{0, cursor.error().to_string(), "", 0, 0, 0, 0});
      } else {
        out = encode_ok(static_cast<uint64_t>(cursor.affected_rows()),
                        session.in_transaction(), cursor.root()->is_write(),
                        session.current_database().str());
      }
    }

    bool ok = false;
    co_await write_all(fd, std::move(out), ok);
    if (error_response) {
      metrics_.errors.fetch_add(1);
    }
    metrics_.rows_sent.fetch_add(rows_sent);
    alive = ok;
  }

  // 收尾：发 BYE（对方可能已经关了，忽略失败）-> 关 fd -> 注销
  bool ignored = false;
  co_await write_all(fd, encode_simple(FrameType::kBye), ignored);
  ::close(fd);
  unregister_connection(fd);
  connections_.fetch_sub(1);
  log("debug", fmt::format("connection closed: fd={}", fd));
}

} // namespace server
