// server/server.cpp
#include "server.h"

#include <poll.h>

#include <utility>

#include <fmt/format.h>

#include "protocol.h"

namespace server {
namespace {

constexpr uint16_t kServerVersion = 1;

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

common::svrkit::TcpServerOptions
Server::make_transport_options(const ServerConfig &config, Server *owner) {
  common::svrkit::TcpServerOptions options;
  options.max_connections = config.max_connections();
  options.logger = [owner](const char *level, const std::string &message) {
    owner->log(level, message);
  };
  return options;
}

Server::Server(ServerConfig config, std::shared_ptr<kv::KVStore> store)
    : config_(std::move(config)), store_(std::move(store)),
      transport_(make_transport_options(config_, this),
                 [this](std::shared_ptr<common::svrkit::TcpConnection>
                            connection) -> common::svrkit::Task {
                   return serve_connection(std::move(connection));
                 }),
      parse_service_("parse-service"), write_service_("write-service") {
}

Server::~Server() {
  transport_.stop();
  parse_service_.stop();
  write_service_.stop();
  for (auto &worker : read_pool_) {
    worker->stop();
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
  if (rank(level) > rank(config_.log_level())) {
    return;
  }
  fmt::print(stderr, "[{}] {}\n", level, message);
}

void Server::arm_idle_watchdog(const std::shared_ptr<ConnState> &state,
                               bool in_tx) {
  const int64_t timeout_ms =
      in_tx ? config_.idle_in_transaction_timeout_ms()
            : config_.idle_timeout_ms();
  if (timeout_ms <= 0) {
    return; // 0 = 不超时
  }
  const uint64_t generation = state->generation.fetch_add(1) + 1;
  transport_.loop().add_timer(timeout_ms, [this, state, generation] {
    if (state->generation.load() != generation ||
        state->connection->stopping()) {
      return; // 连接已经往前走了（读到新语句）或正在收尾
    }
    state->timed_out = true;
    metrics_.idle_timeouts.fetch_add(1);
    // 只关**读方向**：协程会从 read 里醒来，还能把 ERROR 帧写回去
    state->connection->shutdown_read();
  });
}

std::expected<void, std::string> Server::listen() {
  if (store_ == nullptr || !store_->is_open()) {
    return std::unexpected("storage is not open");
  }
  const int requested_port = std::stoi(config_.listen_port());
  return transport_.listen(config_.listen_host(),
                           static_cast<uint16_t>(requested_port));
}

void Server::run() {
  const size_t queue_max = config_.write_queue_max();
  parse_service_.start(queue_max);
  write_service_.start(queue_max);
  // 读线程池：纯读语句真正并发（N = execution.read_threads）
  {
    const size_t read_threads = config_.read_threads();
    const size_t read_queue_max = config_.read_queue_max();
    read_pool_.reserve(read_threads);
    for (size_t i = 0; i < read_threads; ++i) {
      auto worker = std::make_unique<common::svrkit::ServiceThread>(
          fmt::format("read-service-{}", i));
      worker->start(read_queue_max);
      read_pool_.push_back(std::move(worker));
    }
  }
  transport_.run();
  log("info",
      fmt::format("stopped: connections={} statements={} errors={} rows={} "
                  "idle_timeouts={} write_rejects={} parse_rejects={} "
                  "read_rejects={}",
                  metrics_.connections_total.load(), metrics_.statements.load(),
                  metrics_.errors.load(), metrics_.rows_sent.load(),
                  metrics_.idle_timeouts.load(),
                  metrics_.write_queue_rejected.load(),
                  metrics_.parse_queue_rejected.load(),
                  metrics_.read_queue_rejected.load()));
  parse_service_.stop();
  write_service_.stop();
  for (auto &worker : read_pool_) {
    worker->stop();
  }
}

common::svrkit::Task Server::serve_connection(
    std::shared_ptr<common::svrkit::TcpConnection> connection) {
  metrics_.connections_total.fetch_add(1);
  auto state = std::make_shared<ConnState>();
  state->connection = connection;
  const int fd = connection->fd();

  session::Session session(store_->connect());
  const std::string default_database = config_.default_database();
  if (!default_database.empty()) {
    (void)session.execute("USE " + default_database);
  }

  std::string in;
  bool alive = true;
  // 1) HELLO（协议版本 + 能力位）
  {
    bool ok = false;
    co_await connection->write_all(encode_hello(kServerVersion, 0), ok);
    alive = ok;
  }

  while (alive && !transport_.loop().stopped()) {
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
      co_await connection->read_some(in, nread);
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
          co_await connection->write_all(encode_error(timeout), ignored);
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
      co_await connection->write_all(encode_simple(FrameType::kPing), ok);
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
          co_await connection->write_all(encode_error(missing), ok);
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
      co_await connection->write_all(encode_meta_reply(payload), ok);
      alive = ok;
      continue;
    }
    if (frame.type != FrameType::kQuery) {
      bool ok = false;
      ErrorFrame bad;
      bad.message = "unexpected frame from client";
      co_await connection->write_all(encode_error(bad), ok);
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
      common::svrkit::SubmitToService submit;
      submit.service = &parse_service_;
      submit.work = [&session, &sql, &parsed] { parsed = session.parse(sql); };
      co_await submit;
      if (!submit.submitted) {
        metrics_.parse_queue_rejected.fetch_add(1);
        bool ok = false;
        ErrorFrame busy;
        busy.message = "server busy: parse queue is full";
        co_await connection->write_all(encode_error(busy), ok);
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
      // 路由：事务里的语句 + 非只读语句 → 写服务线程；只读且不在事务 → 读池
      const bool route_to_write =
          session.in_transaction() || !parsed->read_only;
      if (route_to_write) {
        common::svrkit::SubmitToService submit;
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
          co_await connection->write_all(encode_error(busy), ok);
          alive = ok;
          continue;
        }
      } else if (!read_pool_.empty()) {
        // 纯读：轮询挑一条读线程执行，读请求之间真正并发
        common::svrkit::ServiceThread &worker =
            *read_pool_[read_next_.fetch_add(1, std::memory_order_relaxed) %
                        read_pool_.size()];
        common::svrkit::SubmitToService submit;
        submit.service = &worker;
        submit.work = [&session, &parsed, &result] {
          result = session.execute_parsed(*parsed);
        };
        co_await submit;
        if (!submit.submitted) {
          metrics_.read_queue_rejected.fetch_add(1);
          bool ok = false;
          ErrorFrame busy;
          busy.message = "server busy: read queue is full";
          co_await connection->write_all(encode_error(busy), ok);
          alive = ok;
          continue;
        }
      } else {
        result = session.execute_parsed(*parsed);
      }
    }

    // 4) 回结果：ERROR / COLUMNS+ROW* / OK
    const size_t max_rows = config_.max_result_rows();
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
          if (++rows > max_rows) {
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
                             std::to_string(max_rows) + ")";
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
    co_await connection->write_all(std::move(out), ok);
    if (error_response) {
      metrics_.errors.fetch_add(1);
    }
    metrics_.rows_sent.fetch_add(rows_sent);
    alive = ok;
  }

  // 协议收尾：发 BYE（对方可能已经关了，忽略失败）。
  // fd 的关闭与连接注销由 svrkit::TcpServer 在处理器返回后统一完成。
  bool ignored = false;
  co_await connection->write_all(encode_simple(FrameType::kBye), ignored);
  log("debug", fmt::format("connection closed: fd={}", fd));
}

} // namespace server
