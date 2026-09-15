// client/connection.cpp
#include "connection.h"

#include <unistd.h>

#include <cstring>
#include <charconv>
#include <utility>

#include "common/net/socket.h"
#include "common/net/socket_util.h"
#include "common/proto/protocol.h"
#include "session/session.h"

namespace client {
namespace {

// ---- 本地连接：直接包 session::Session ----
class LocalConnection : public SqlConnection {
public:
  explicit LocalConnection(std::shared_ptr<kv::KVEngine> engine)
      : session_(std::move(engine)) {}

  Outcome execute(const std::string &sql) override {
    Outcome outcome;
    auto result = session_.execute(sql);
    outcome.in_transaction = session_.in_transaction();
    if (!result.has_value()) {
      fill_error(&outcome, result.error());
      return outcome;
    }
    exec::ResultCursor &cursor = **result;
    outcome.has_rows = cursor.root()->produces_rows();
    outcome.is_write = cursor.root()->is_write();
    if (!outcome.has_rows) {
      if (cursor.error().is_error()) {
        outcome.error_code = static_cast<uint8_t>(cursor.error().code);
        outcome.error_message = cursor.error().to_string();
        return outcome;
      }
      outcome.ok = true;
      outcome.affected_rows = cursor.affected_rows();
      return outcome;
    }
    outcome.columns = cursor.columns();
    while (true) {
      auto row = cursor.next();
      if (!row.has_value()) {
        if (row.error().is_error()) {
          outcome.error_code = static_cast<uint8_t>(row.error().code);
          outcome.error_message = row.error().to_string();
          return outcome;
        }
        break;
      }
      std::vector<Cell> cells;
      cells.reserve(cursor.columns().size());
      for (size_t i = 0; i < cursor.columns().size(); ++i) {
        Cell cell;
        if (i < row->size()) {
          cell.is_null = (*row)[i].is_null();
          if (!cell.is_null) {
            cell.text = (*row)[i].to_string();
          }
        }
        cells.push_back(std::move(cell));
      }
      outcome.rows.push_back(std::move(cells));
    }
    outcome.ok = true;
    return outcome;
  }

  std::string current_database() const override {
    return session_.current_database().str();
  }
  bool in_transaction() const override { return session_.in_transaction(); }
  bool supports_metadata() const override { return true; }

  std::vector<DatabaseMeta> databases() override {
    std::vector<DatabaseMeta> out;
    for (const auto &info : session_.databases()) {
      DatabaseMeta meta;
      meta.name = info.name.str();
      meta.created_at = info.created_at;
      meta.table_count = info.table_count;
      meta.is_current = info.is_current;
      out.push_back(std::move(meta));
    }
    return out;
  }

  std::vector<TableMeta> tables(const std::string &db) override {
    std::vector<TableMeta> out;
    for (const auto &info : session_.tables(sql::Identifier(db))) {
      TableMeta meta;
      meta.name = info.name.str();
      meta.column_count = info.column_count;
      meta.primary_key = info.primary_key.str();
      meta.created_at = info.created_at;
      meta.last_write_at = info.last_write_at;
      meta.row_count = info.row_count;
      out.push_back(std::move(meta));
    }
    return out;
  }

  std::optional<sql::TableSchema> table_schema(const std::string &table,
                                               const std::string &db) override {
    return session_.table_schema(sql::Identifier(table), sql::Identifier(db));
  }

  std::string description() const override { return "local"; }

private:
  static void fill_error(Outcome *outcome, const session::SessionError &error) {
    outcome->error_code = static_cast<uint8_t>(error.code);
    outcome->error_message = error.to_string();
    outcome->error_sql = error.sql;
    outcome->error_span = error.span;
  }

  session::Session session_;
};

// ---- 远程连接：阻塞式协议客户端 ----
class RemoteConnection : public SqlConnection {
public:
  using Dialer = std::function<std::expected<int, std::string>(
      const std::string &host, const std::string &port)>;

  RemoteConnection(common::net::TcpSocket socket, std::string description,
                   Dialer dialer = {})
      : socket_(std::move(socket)), description_(std::move(description)),
        dialer_(std::move(dialer)) {}

  Outcome execute(const std::string &sql) override {
    Outcome outcome;
    if (!send_all(common::proto::encode_query(sql))) {
      return connection_lost(outcome);
    }
    while (true) {
      auto frame = next_frame();
      if (!frame.has_value()) {
        return connection_lost(outcome);
      }
      switch (frame->type) {
      case common::proto::FrameType::kColumns:
        if (!common::proto::decode_columns(frame->payload, &outcome.columns)) {
          return connection_lost(outcome);
        }
        break;
      case common::proto::FrameType::kRow: {
        std::vector<common::proto::ProtocolValue> row;
        if (!common::proto::decode_row(frame->payload, &row)) {
          return connection_lost(outcome);
        }
        std::vector<Cell> cells;
        cells.reserve(row.size());
        for (auto &value : row) {
          Cell cell;
          cell.is_null = value.is_null;
          cell.text = std::move(value.text);
          cells.push_back(std::move(cell));
        }
        outcome.rows.push_back(std::move(cells));
        break;
      }
      case common::proto::FrameType::kOk: {
        uint8_t flags = 0;
        std::string current_db;
        if (!common::proto::decode_ok(frame->payload, &outcome.affected_rows, &flags,
                               &current_db)) {
          return connection_lost(outcome);
        }
        outcome.in_transaction = (flags & 0x1) != 0;
        outcome.is_write = (flags & 0x2) != 0;
        in_transaction_ = outcome.in_transaction;
        current_db_ = current_db;
        outcome.has_rows = !outcome.columns.empty();
        outcome.ok = true;
        return outcome;
      }
      case common::proto::FrameType::kError: {
        common::proto::ErrorFrame error;
        if (!common::proto::decode_error(frame->payload, &error)) {
          return connection_lost(outcome);
        }
        outcome.ok = false;
        outcome.error_code = error.code;
        outcome.error_message = error.message;
        outcome.error_sql = error.sql;
        outcome.error_span = sspan_make(error.begin_line, error.begin_column,
                                        error.end_line, error.end_column);
        if (error.leader_hint.has_value()) {
          // 只在服务端通告了能力位时才认这个 hint（版本协商纪律：老客户端
          // 遇到新服务端也不会误读尾部字段）。
          if ((capabilities_ & common::proto::kCapabilityLeaderHint) != 0) {
            outcome.redirect_node_id = error.leader_hint->node_id;
            outcome.redirect_endpoint = error.leader_hint->endpoint;
          }
        }
        outcome.in_transaction = in_transaction_;
        return outcome;
      }
      default:
        return connection_lost(outcome);
      }
    }
  }

  std::string current_database() const override { return current_db_; }
  bool in_transaction() const override { return in_transaction_; }

  // 换到 leader 节点：建新连接 -> 握手 -> 恢复当前库。
  //
  // 只在**不在事务里**时才有意义（事务状态在旧连接上，换过去就没了）；
  // 调用方（REPL）负责这个判断，这里只做连接层的事。
  bool redirect(const std::string &endpoint, std::string *error) override {
    const size_t colon = endpoint.rfind(':');
    if (colon == std::string::npos || colon == 0 ||
        colon + 1 >= endpoint.size()) {
      if (error != nullptr) {
        *error = "leader endpoint is not host:port: " + endpoint;
      }
      return false;
    }
    const std::string host = endpoint.substr(0, colon);
    const std::string port = endpoint.substr(colon + 1);
    // 地址来自服务端（配置里校验过，但也可能是别人），用 from_chars 解析：
    // 非数字端口要报错而不是抛异常。
    uint32_t port_number = 0;
    const char *begin = port.data();
    const char *end = begin + port.size();
    const auto rc = std::from_chars(begin, end, port_number, 10);
    if (port.empty() || rc.ec != std::errc() || rc.ptr != end ||
        port_number == 0 || port_number > 65535) {
      if (error != nullptr) {
        *error = "leader endpoint has an invalid port: " + endpoint;
      }
      return false;
    }

    std::expected<int, std::string> fd = std::unexpected(
        std::string{"no dialer"});
    if (dialer_) {
      fd = dialer_(host, port);
    } else {
      auto socket = common::net::TcpSocket::connect(
          host, static_cast<uint16_t>(port_number));
      if (socket.has_value()) {
        fd = socket->release();
      } else {
        fd = std::unexpected(socket.error());
      }
    }
    if (!fd.has_value()) {
      if (error != nullptr) {
        *error = "cannot connect to " + endpoint + ": " + fd.error();
      }
      return false;
    }

    common::net::TcpSocket socket = common::net::TcpSocket::adopt(*fd);
    common::net::socket_suppress_sigpipe(socket.fd());
    auto replacement = std::make_unique<RemoteConnection>(
        std::move(socket), endpoint, dialer_);
    if (!replacement->handshake()) {
      if (error != nullptr) {
        *error = "handshake with " + endpoint + " failed";
      }
      return false;
    }
    // 会话状态：当前库跟着走，事务不跟（调用方保证这里没有事务）。
    if (!current_db_.empty()) {
      const Outcome used = replacement->execute("USE " + current_db_);
      if (!used.ok) {
        if (error != nullptr) {
          *error = "cannot restore current database on " + endpoint;
        }
        return false;
      }
    }

    socket_ = std::move(replacement->socket_);
    in_.clear();
    description_ = endpoint;
    protocol_version_ = replacement->protocol_version_;
    server_version_ = replacement->server_version_;
    in_transaction_ = false;
    return true;
  }

  bool supports_metadata() const override { return true; }

  std::vector<DatabaseMeta> databases() override {
    const std::optional<std::string> payload =
        meta_request(common::proto::MetaKind::kDatabases, "", "");
    std::vector<DatabaseMeta> out;
    if (!payload.has_value()) {
      return out;
    }
    std::vector<common::proto::MetaDatabase> decoded;
    if (!common::proto::decode_meta_databases(*payload, &decoded)) {
      return out;
    }
    for (auto &db : decoded) {
      DatabaseMeta meta;
      meta.name = std::move(db.name);
      meta.created_at = db.created_at;
      meta.table_count = db.table_count;
      meta.is_current = db.is_current;
      out.push_back(std::move(meta));
    }
    return out;
  }

  std::vector<TableMeta> tables(const std::string &db) override {
    const std::optional<std::string> payload =
        meta_request(common::proto::MetaKind::kTables, db, "");
    std::vector<TableMeta> out;
    if (!payload.has_value()) {
      return out;
    }
    std::vector<common::proto::MetaTable> decoded;
    if (!common::proto::decode_meta_tables(*payload, &decoded)) {
      return out;
    }
    for (auto &table : decoded) {
      TableMeta meta;
      meta.name = std::move(table.name);
      meta.column_count = table.column_count;
      meta.primary_key = std::move(table.primary_key);
      meta.created_at = table.created_at;
      meta.last_write_at = table.last_write_at;
      meta.row_count = table.row_count;
      out.push_back(std::move(meta));
    }
    return out;
  }

  std::optional<sql::TableSchema> table_schema(const std::string &table,
                                               const std::string &db) override {
    const std::optional<std::string> payload =
        meta_request(common::proto::MetaKind::kSchema, db, table);
    if (!payload.has_value()) {
      return std::nullopt;
    }
    sql::TableSchema schema;
    if (!common::proto::decode_meta_schema(*payload, &schema)) {
      return std::nullopt;
    }
    return schema;
  }

  std::string description() const override { return description_; }

  // 连上之后先读 HELLO（协议版本/能力位）：**必须在任何 QUERY 之前消费掉**，
  // 否则第一条语句会把 HELLO 当成"不该出现的帧"而报连接断开。
  bool handshake() {
    auto frame = next_frame();
    if (!frame.has_value() || frame->type != common::proto::FrameType::kHello) {
      return false;
    }
    uint16_t proto = 0;
    uint16_t server_version = 0;
    uint32_t capabilities = 0;
    if (!common::proto::decode_hello(frame->payload, &proto, &server_version,
                              &capabilities)) {
      return false;
    }
    protocol_version_ = proto;
    server_version_ = server_version;
    capabilities_ = capabilities;
    return proto == common::proto::kProtocolVersion;
  }

private:
  // 发一个 META 请求并等回复；表不存在等错误返回 nullopt
  std::optional<std::string> meta_request(common::proto::MetaKind kind,
                                          const std::string &arg1,
                                          const std::string &arg2) {
    if (!send_all(common::proto::encode_meta(kind, arg1, arg2))) {
      return std::nullopt;
    }
    while (true) {
      auto frame = next_frame();
      if (!frame.has_value()) {
        return std::nullopt;
      }
      if (frame->type == common::proto::FrameType::kMetaReply) {
        std::string payload;
        if (!common::proto::decode_meta_reply(frame->payload, &payload)) {
          return std::nullopt;
        }
        return payload;
      }
      if (frame->type == common::proto::FrameType::kError) {
        return std::nullopt;
      }
      return std::nullopt; // 不该出现的帧
    }
  }

  static Outcome connection_lost(Outcome outcome) {
    outcome.ok = false;
    outcome.error_code = 255;
    outcome.error_message = "connection to server lost";
    return outcome;
  }

  bool send_all(const std::string &data) {
    return socket_.send_all(data);
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
      const ssize_t got = socket_.read_once(chunk, sizeof(chunk));
      if (got <= 0) {
        return std::nullopt;
      }
      in_.append(chunk, static_cast<size_t>(got));
    }
  }

  common::net::TcpSocket socket_;
  std::string in_;
  std::string description_;
  Dialer dialer_;
  std::string current_db_;
  bool in_transaction_ = false;
  uint16_t protocol_version_ = 0;
  uint16_t server_version_ = 0;
  uint32_t capabilities_ = 0;
};

std::unique_ptr<SqlConnection>
make_remote_from_socket(common::net::TcpSocket socket, std::string *error,
                        const std::string &label,
                        RemoteConnection::Dialer dialer = {}) {
  auto connection = std::make_unique<RemoteConnection>(std::move(socket), label,
                                                       std::move(dialer));
  if (!connection->handshake()) {
    if (error != nullptr) {
      *error = "protocol handshake failed (server speaks a different version?)";
    }
    return nullptr;
  }
  return connection;
}

} // namespace

std::unique_ptr<SqlConnection>
make_local(std::shared_ptr<kv::KVEngine> engine) {
  return std::make_unique<LocalConnection>(std::move(engine));
}

std::unique_ptr<SqlConnection> make_remote(const RemoteOptions &options,
                                           std::string *error) {
  common::net::TcpSocket socket;
  if (options.dialer) {
    auto fd = options.dialer(options.host, options.port);
    if (!fd.has_value()) {
      if (error != nullptr) {
        *error = fd.error();
      }
      return nullptr;
    }
    socket = common::net::TcpSocket::adopt(*fd);
    common::net::socket_suppress_sigpipe(socket.fd());
  } else {
    auto connected = common::net::TcpSocket::connect(
        options.host, static_cast<uint16_t>(std::stoi(options.port)));
    if (!connected.has_value()) {
      if (error != nullptr) {
        *error = connected.error();
      }
      return nullptr;
    }
    socket = std::move(*connected);
  }
  const std::string description = options.host + ":" + options.port;
  return make_remote_from_socket(std::move(socket), error, description,
                                 options.dialer);
}

std::unique_ptr<SqlConnection> make_remote_from_fd(int fd, std::string *error,
                                                   const std::string &label) {
  common::net::TcpSocket socket = common::net::TcpSocket::adopt(fd);
  common::net::socket_suppress_sigpipe(socket.fd());
  return make_remote_from_socket(std::move(socket), error, label);
}

} // namespace client
