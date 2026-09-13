// client/connection.cpp
#include "connection.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cstring>
#include <utility>

#include "server/protocol.h"
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
  RemoteConnection(int fd, std::string description)
      : fd_(fd), description_(std::move(description)) {}
  ~RemoteConnection() override {
    if (fd_ >= 0) {
      ::close(fd_);
    }
  }

  Outcome execute(const std::string &sql) override {
    Outcome outcome;
    if (!send_all(server::encode_query(sql))) {
      return connection_lost(outcome);
    }
    while (true) {
      auto frame = next_frame();
      if (!frame.has_value()) {
        return connection_lost(outcome);
      }
      switch (frame->type) {
      case server::FrameType::kColumns:
        if (!server::decode_columns(frame->payload, &outcome.columns)) {
          return connection_lost(outcome);
        }
        break;
      case server::FrameType::kRow: {
        std::vector<server::ProtocolValue> row;
        if (!server::decode_row(frame->payload, &row)) {
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
      case server::FrameType::kOk: {
        uint8_t flags = 0;
        std::string current_db;
        if (!server::decode_ok(frame->payload, &outcome.affected_rows, &flags,
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
      case server::FrameType::kError: {
        server::ErrorFrame error;
        if (!server::decode_error(frame->payload, &error)) {
          return connection_lost(outcome);
        }
        outcome.ok = false;
        outcome.error_code = error.code;
        outcome.error_message = error.message;
        outcome.error_sql = error.sql;
        outcome.error_span = sspan_make(error.begin_line, error.begin_column,
                                        error.end_line, error.end_column);
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

  bool supports_metadata() const override { return true; }

  std::vector<DatabaseMeta> databases() override {
    const std::optional<std::string> payload =
        meta_request(server::MetaKind::kDatabases, "", "");
    std::vector<DatabaseMeta> out;
    if (!payload.has_value()) {
      return out;
    }
    std::vector<server::MetaDatabase> decoded;
    if (!server::decode_meta_databases(*payload, &decoded)) {
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
        meta_request(server::MetaKind::kTables, db, "");
    std::vector<TableMeta> out;
    if (!payload.has_value()) {
      return out;
    }
    std::vector<server::MetaTable> decoded;
    if (!server::decode_meta_tables(*payload, &decoded)) {
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
        meta_request(server::MetaKind::kSchema, db, table);
    if (!payload.has_value()) {
      return std::nullopt;
    }
    sql::TableSchema schema;
    if (!server::decode_meta_schema(*payload, &schema)) {
      return std::nullopt;
    }
    return schema;
  }

  std::string description() const override { return description_; }

  // 连上之后先读 HELLO（协议版本/能力位）：**必须在任何 QUERY 之前消费掉**，
  // 否则第一条语句会把 HELLO 当成"不该出现的帧"而报连接断开。
  bool handshake() {
    auto frame = next_frame();
    if (!frame.has_value() || frame->type != server::FrameType::kHello) {
      return false;
    }
    uint16_t proto = 0;
    uint16_t server_version = 0;
    uint32_t capabilities = 0;
    if (!server::decode_hello(frame->payload, &proto, &server_version,
                              &capabilities)) {
      return false;
    }
    protocol_version_ = proto;
    server_version_ = server_version;
    return proto == server::kProtocolVersion;
  }

private:
  // 发一个 META 请求并等回复；表不存在等错误返回 nullopt
  std::optional<std::string> meta_request(server::MetaKind kind,
                                          const std::string &arg1,
                                          const std::string &arg2) {
    if (!send_all(server::encode_meta(kind, arg1, arg2))) {
      return std::nullopt;
    }
    while (true) {
      auto frame = next_frame();
      if (!frame.has_value()) {
        return std::nullopt;
      }
      if (frame->type == server::FrameType::kMetaReply) {
        std::string payload;
        if (!server::decode_meta_reply(frame->payload, &payload)) {
          return std::nullopt;
        }
        return payload;
      }
      if (frame->type == server::FrameType::kError) {
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
    size_t sent = 0;
    while (sent < data.size()) {
      const ssize_t wrote =
          ::write(fd_, data.data() + sent, data.size() - sent);
      if (wrote <= 0) {
        return false;
      }
      sent += static_cast<size_t>(wrote);
    }
    return true;
  }

  std::optional<server::DecodedFrame> next_frame() {
    while (true) {
      server::DecodedFrame frame;
      size_t consumed = 0;
      std::string error;
      if (server::try_decode_frame(in_, &frame, &consumed, &error)) {
        in_.erase(0, consumed);
        return frame;
      }
      if (!error.empty()) {
        return std::nullopt;
      }
      char chunk[8192];
      const ssize_t got = ::read(fd_, chunk, sizeof(chunk));
      if (got <= 0) {
        return std::nullopt;
      }
      in_.append(chunk, static_cast<size_t>(got));
    }
  }

  int fd_ = -1;
  std::string in_;
  std::string description_;
  std::string current_db_;
  bool in_transaction_ = false;
  uint16_t protocol_version_ = 0;
  uint16_t server_version_ = 0;
};

} // namespace

std::unique_ptr<SqlConnection>
make_local(std::shared_ptr<kv::KVEngine> engine) {
  return std::make_unique<LocalConnection>(std::move(engine));
}

std::unique_ptr<SqlConnection> make_remote(const RemoteOptions &options,
                                           std::string *error) {
  const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0) {
    if (error != nullptr) {
      *error = std::string("socket(): ") + std::strerror(errno);
    }
    return nullptr;
  }
  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_port = htons(static_cast<uint16_t>(std::stoi(options.port)));
  if (::inet_pton(AF_INET, options.host.c_str(), &addr.sin_addr) != 1) {
    if (error != nullptr) {
      *error = "host must be an IPv4 address: " + options.host;
    }
    ::close(fd);
    return nullptr;
  }
  if (::connect(fd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) != 0) {
    if (error != nullptr) {
      *error = "connect " + options.host + ":" + options.port +
               " failed: " + std::strerror(errno);
    }
    ::close(fd);
    return nullptr;
  }
  const std::string description = options.host + ":" + options.port;
  return make_remote_from_fd(fd, error, description);
}

std::unique_ptr<SqlConnection> make_remote_from_fd(int fd, std::string *error,
                                                   const std::string &label) {
  auto connection = std::make_unique<RemoteConnection>(fd, label);
  if (!connection->handshake()) {
    if (error != nullptr) {
      *error = "protocol handshake failed (server speaks a different version?)";
    }
    return nullptr;
  }
  return connection;
}

} // namespace client
