// server/config.cpp
#include "config.h"

#include <cctype>
#include <charconv>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <utility>

#include "raft/peers.h"

// ============================================================
// 取值宏：把字段名字符串化（#field），是 ServerConfig 方法实现的内部
// 工具 —— 只定义在这一个 .cpp 里，调用点看到的是 ServerConfig 的方法，
// 看不到宏。
// ============================================================
#define CFG_STR(cfg, field, ...)                                               \
  (cfg).get_string(#field __VA_OPT__(, ) __VA_ARGS__)
#define CFG_INT(cfg, field, ...)                                               \
  (cfg).get_int(#field __VA_OPT__(, ) __VA_ARGS__)
#define CFG_BOOL(cfg, field, ...)                                              \
  (cfg).get_bool(#field __VA_OPT__(, ) __VA_ARGS__)

namespace server {
namespace {

// 内置默认值：同时充当 schema —— validate() 用它核对"已知 section 里有
// 哪些合法 key、各 key 应该是什么类型"。加配置字段 = 在这里加一行 +
// （可选）在 validate() 里加范围/枚举规则。
constexpr std::string_view kDefaultsIni = R"ini(
[server]
listen = 127.0.0.1:5433
max_connections = 256
idle_timeout_ms = 300000
idle_in_transaction_timeout_ms = 30000
log_level = info
log_file =

[storage]
engine = leveldb
path = ./sql_db
create_if_missing = true

[execution]
read_threads = 1
read_queue_max = 1024
write_queue_max = 1024
max_result_rows = 1000000
statement_timeout_ms = 0

[session]
default_database =

[raft]
# 默认关闭：不开 raft 时启动路径与以前完全一致（直接用本地 KVStore）。
enabled = false
node_id = 1
listen = 127.0.0.1:5434
# 静态成员表：node_id@host:port，逗号分隔，必须包含本节点的 node_id。
peers =
# 客户端可达的 SQL 地址（node_id@host:port）：本节点不是 leader 时用它回
# "NotLeader + 该去哪台"，客户端据此重连。留空 = 只报错、不给重定向目标。
sql_endpoints =
election_timeout_ms = 1000
heartbeat_ms = 100
# Raft 日志/HardState 的独立 LevelDB 目录（不要和业务数据共用一个目录）。
log_path =
# 日志条目数超过该值时，leader 生成快照并压缩日志（0 = 关闭压缩）。
# 压缩后落后者/新节点靠 InstallSnapshot 追赶。
snapshot_entries = 0
)ini";

std::string trim(const std::string &text) {
  size_t begin = 0;
  while (begin < text.size() &&
         std::isspace(static_cast<unsigned char>(text[begin])) != 0) {
    ++begin;
  }
  size_t end = text.size();
  while (end > begin &&
         std::isspace(static_cast<unsigned char>(text[end - 1])) != 0) {
    --end;
  }
  return text.substr(begin, end - begin);
}

std::string lower(std::string text) {
  for (char &c : text) {
    c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  }
  return text;
}

std::string fail(size_t line, const std::string &what) {
  return "line " + std::to_string(line) + ": " + what;
}

// 类型嗅探：bool 关键字 → BOOLEAN；纯整数 → BIGINT；其余 → VARCHAR。
// 值的大小写保留（路径/库名区分大小写），嗅探只看不改。
sql::Value sniff(const std::string &value) {
  const std::string v = lower(value);
  if (v == "true" || v == "yes" || v == "on") {
    return sql::Value(true);
  }
  if (v == "false" || v == "no" || v == "off") {
    return sql::Value(false);
  }
  int64_t parsed = 0;
  const char *begin = value.data();
  const char *end = begin + value.size();
  const auto rc = std::from_chars(begin, end, parsed, 10);
  if (rc.ec == std::errc() && rc.ptr == end) {
    return sql::Value(parsed);
  }
  return sql::Value(value);
}

std::string type_name(const sql::Value &value) {
  if (value.is_bool()) {
    return "bool";
  }
  if (value.is_int()) {
    return "int";
  }
  if (value.is_string()) {
    return "string";
  }
  return "other";
}

std::string type_error(std::string_view dotted, const sql::Value &value,
                       const char *expected) {
  return "config key '" + std::string(dotted) + "': expected " + expected +
         ", got " + type_name(value);
}

// "a.b" → ("a", "b")；没有点 → ("", 整个)
std::pair<std::string_view, std::string_view>
split_key(std::string_view dotted) {
  const size_t dot = dotted.find('.');
  if (dot == std::string_view::npos) {
    return {{}, dotted};
  }
  return {dotted.substr(0, dot), dotted.substr(dot + 1)};
}

// 纯语法解析：section/key 小写化，值做类型嗅探；同键重复后写覆盖先写。
std::expected<Config::Table, std::string> parse_ini(const std::string &text) {
  Config::Table table;
  std::string section;
  std::istringstream input(text);
  std::string line;
  size_t line_no = 0;

  while (std::getline(input, line)) {
    ++line_no;
    const std::string raw = trim(line);
    if (raw.empty() || raw[0] == '#' || raw[0] == ';') {
      continue;
    }
    if (raw.front() == '[') {
      if (raw.back() != ']') {
        return std::unexpected(fail(line_no, "section header missing ']'"));
      }
      section = lower(trim(raw.substr(1, raw.size() - 2)));
      continue;
    }
    const size_t eq = raw.find('=');
    if (eq == std::string::npos) {
      return std::unexpected(fail(line_no, "expected 'key = value'"));
    }
    const std::string key = lower(trim(raw.substr(0, eq)));
    if (key.empty()) {
      return std::unexpected(fail(line_no, "empty key"));
    }
    const std::string value = trim(raw.substr(eq + 1));
    table[section][key] = sniff(value);
  }
  return table;
}

const Config::Table &defaults_table() {
  static const Config::Table table = [] {
    auto parsed = parse_ini(std::string(kDefaultsIni));
    // 内置常量解析失败 = 代码 bug，没法恢复
    if (!parsed.has_value()) {
      throw std::logic_error("built-in config defaults do not parse: " +
                             parsed.error());
    }
    return std::move(*parsed);
  }();
  return table;
}

} // namespace

Config::Config() : table_(defaults_table()) {}

const sql::Value *Config::find(std::string_view dotted) const {
  const auto [section, key] = split_key(dotted);
  const auto sit = table_.find(section);
  if (sit == table_.end()) {
    return nullptr;
  }
  const auto kit = sit->second.find(key);
  return kit == sit->second.end() ? nullptr : &kit->second;
}

std::string Config::get_string(std::string_view dotted,
                               std::string fallback) const {
  const sql::Value *value = find(dotted);
  if (value == nullptr) {
    return fallback;
  }
  if (!value->is_string()) {
    throw std::runtime_error(type_error(dotted, *value, "string"));
  }
  return value->as_str();
}

int64_t Config::get_int(std::string_view dotted, int64_t fallback) const {
  const sql::Value *value = find(dotted);
  if (value == nullptr) {
    return fallback;
  }
  if (!value->is_int()) {
    throw std::runtime_error(type_error(dotted, *value, "int"));
  }
  return value->as_int();
}

bool Config::get_bool(std::string_view dotted, bool fallback) const {
  const sql::Value *value = find(dotted);
  if (value == nullptr) {
    return fallback;
  }
  if (!value->is_bool()) {
    throw std::runtime_error(type_error(dotted, *value, "bool"));
  }
  return value->as_bool();
}

void Config::set(std::string_view dotted, sql::Value value) {
  const auto [section, key] = split_key(dotted);
  table_[std::string(section)][std::string(key)] = std::move(value);
}

void Config::merge(const Table &overlay) {
  for (const auto &[section, entries] : overlay) {
    for (const auto &[key, value] : entries) {
      table_[section][key] = value;
    }
  }
}

std::expected<void, std::string> Config::validate() const {
  const Table &schema = defaults_table();

  // 1) 已知 section：key 必须在默认值表里（拦拼错），类型必须一致。
  //    未知 section 是别的组件的自留地，放行。
  for (const auto &[section, entries] : table_) {
    const auto sit = schema.find(section);
    if (sit == schema.end()) {
      continue;
    }
    for (const auto &[key, value] : entries) {
      const auto kit = sit->second.find(key);
      if (kit == sit->second.end()) {
        return std::unexpected("unknown key: " + section + "." + key);
      }
      if (value.type() != kit->second.type()) {
        return std::unexpected(type_error(section + "." + key, value,
                                          type_name(kit->second).c_str()));
      }
    }
  }

  // 2) 范围（类型已被 1) 保证，get_int 不会抛）
  struct IntRule {
    std::string_view key;
    int64_t min;
    int64_t max;
  };
  constexpr IntRule kIntRules[] = {
      {"server.max_connections", 1, INT64_MAX},
      {"server.idle_timeout_ms", 0, INT64_MAX},
      {"server.idle_in_transaction_timeout_ms", 0, INT64_MAX},
      {"execution.read_threads", 1, 64},
      {"execution.read_queue_max", 1, INT64_MAX},
      {"execution.write_queue_max", 1, INT64_MAX},
      {"execution.max_result_rows", 1, INT64_MAX},
      {"execution.statement_timeout_ms", 0, INT64_MAX},
      {"raft.node_id", 1, INT64_MAX},
      {"raft.election_timeout_ms", 1, INT64_MAX},
      {"raft.heartbeat_ms", 1, INT64_MAX},
      {"raft.snapshot_entries", 0, INT64_MAX},
  };
  for (const auto &rule : kIntRules) {
    const int64_t v = get_int(rule.key);
    if (v < rule.min || v > rule.max) {
      std::string message = "config key '" + std::string(rule.key) + "': ";
      message += rule.max == INT64_MAX
                     ? "must be >= " + std::to_string(rule.min)
                     : "must be in [" + std::to_string(rule.min) + ", " +
                           std::to_string(rule.max) + "]";
      return std::unexpected(message);
    }
  }

  // 3) 枚举与形状
  const auto check_enum =
      [this](std::string_view dotted,
             std::initializer_list<std::string_view> allowed)
      -> std::expected<void, std::string> {
    const std::string v = get_string(dotted);
    std::string joined;
    for (const std::string_view one : allowed) {
      if (v == one) {
        return {};
      }
      joined += joined.empty() ? std::string(one) : "|" + std::string(one);
    }
    return std::unexpected("config key '" + std::string(dotted) +
                           "': must be " + joined);
  };
  if (auto ok =
          check_enum("server.log_level", {"error", "warn", "info", "debug"});
      !ok.has_value()) {
    return ok;
  }
  if (auto ok = check_enum("storage.engine", {"leveldb", "mock"});
      !ok.has_value()) {
    return ok;
  }

  const std::string listen = get_string("server.listen");
  const size_t colon = listen.rfind(':');
  if (colon == std::string::npos || colon == 0 || colon + 1 >= listen.size()) {
    return std::unexpected(
        "config key 'server.listen': must look like host:port");
  }
  if (get_string("storage.path").empty()) {
    return std::unexpected("config key 'storage.path': must not be empty");
  }

  // [raft]：默认关闭；只在 enabled = true 时要求节点/成员表/日志目录齐全。
  const bool raft_enabled = get_bool("raft.enabled");
  const int64_t election_timeout = get_int("raft.election_timeout_ms");
  const int64_t heartbeat = get_int("raft.heartbeat_ms");
  if (heartbeat >= election_timeout) {
    return std::unexpected(
        "config key 'raft.heartbeat_ms': must be smaller than "
        "raft.election_timeout_ms");
  }
  const std::string raft_peers = get_string("raft.peers");
  if (!raft_enabled) {
    // Still reject a malformed list when it is present, so a typo cannot hide
    // until the day raft is switched on.
    if (!raft_peers.empty()) {
      auto parsed = raft::parse_peer_list(raft_peers);
      if (!parsed.has_value()) {
        return std::unexpected(parsed.error());
      }
    }
    const std::string endpoints = get_string("raft.sql_endpoints");
    if (!endpoints.empty() && !raft::parse_peer_list(endpoints).has_value()) {
      return std::unexpected("config key 'raft.sql_endpoints': " +
                             raft::parse_peer_list(endpoints).error());
    }
    return {};
  }

  if (raft_peers.empty()) {
    return std::unexpected(
        "config key 'raft.peers': must not be empty when raft.enabled = true");
  }
  const auto peers = raft::parse_peer_list(raft_peers);
  if (!peers.has_value()) {
    return std::unexpected(peers.error());
  }
  const uint64_t node_id = static_cast<uint64_t>(get_int("raft.node_id"));
  bool self_found = false;
  for (const auto &peer : *peers) {
    if (peer.node_id.value == node_id) {
      self_found = true;
    }
  }
  if (!self_found) {
    return std::unexpected("config key 'raft.node_id': must appear in "
                           "raft.peers (peers = " +
                           raft_peers + ")");
  }

  // 客户端重定向表：写法与 peers 相同（id@host:port），id 必须是已知 peer
  // —— 拼错的 id 会让重定向静默失效，所以直接拒绝。
  const std::string sql_endpoints = get_string("raft.sql_endpoints");
  if (!sql_endpoints.empty()) {
    const auto endpoints = raft::parse_peer_list(sql_endpoints);
    if (!endpoints.has_value()) {
      return std::unexpected("config key 'raft.sql_endpoints': " +
                             endpoints.error());
    }
    for (const auto &endpoint : *endpoints) {
      bool known = false;
      for (const auto &peer : *peers) {
        if (peer.node_id == endpoint.node_id) {
          known = true;
          break;
        }
      }
      if (!known) {
        return std::unexpected(
            "config key 'raft.sql_endpoints': node id " +
            std::to_string(endpoint.node_id.value) +
            " is not in raft.peers");
      }
    }
  }
  const std::string raft_listen = get_string("raft.listen");
  const size_t raft_colon = raft_listen.rfind(':');
  if (raft_colon == std::string::npos || raft_colon == 0 ||
      raft_colon + 1 >= raft_listen.size()) {
    return std::unexpected(
        "config key 'raft.listen': must look like host:port");
  }
  if (get_string("raft.log_path").empty()) {
    return std::unexpected(
        "config key 'raft.log_path': must not be empty when "
        "raft.enabled = true");
  }
  return {};
}

// ============================================================
// ServerConfig：每个字段一个方法（实现里才碰宏与 dotted key）
// ============================================================

ServerConfig::ServerConfig() = default;

ServerConfig::ServerConfig(Config generic) : generic_(std::move(generic)) {}

void ServerConfig::set(std::string_view dotted, sql::Value value) {
  generic_.set(dotted, std::move(value));
}

std::expected<void, std::string> ServerConfig::validate() const {
  return generic_.validate();
}

std::string ServerConfig::listen() const {
  return CFG_STR(generic_, server.listen);
}

size_t ServerConfig::max_connections() const {
  return static_cast<size_t>(CFG_INT(generic_, server.max_connections));
}

int64_t ServerConfig::idle_timeout_ms() const {
  return CFG_INT(generic_, server.idle_timeout_ms);
}

int64_t ServerConfig::idle_in_transaction_timeout_ms() const {
  return CFG_INT(generic_, server.idle_in_transaction_timeout_ms);
}

std::string ServerConfig::log_level() const {
  return CFG_STR(generic_, server.log_level);
}

std::string ServerConfig::log_file() const {
  return CFG_STR(generic_, server.log_file);
}

std::string ServerConfig::listen_host() const {
  const std::string listen = CFG_STR(generic_, server.listen);
  const size_t colon = listen.rfind(':');
  return colon == std::string::npos ? std::string() : listen.substr(0, colon);
}

std::string ServerConfig::listen_port() const {
  const std::string listen = CFG_STR(generic_, server.listen);
  const size_t colon = listen.rfind(':');
  return colon == std::string::npos ? std::string() : listen.substr(colon + 1);
}

std::string ServerConfig::engine() const {
  return CFG_STR(generic_, storage.engine);
}

std::string ServerConfig::path() const {
  return CFG_STR(generic_, storage.path);
}

bool ServerConfig::create_if_missing() const {
  return CFG_BOOL(generic_, storage.create_if_missing);
}

size_t ServerConfig::read_threads() const {
  return static_cast<size_t>(CFG_INT(generic_, execution.read_threads));
}

size_t ServerConfig::read_queue_max() const {
  return static_cast<size_t>(CFG_INT(generic_, execution.read_queue_max));
}

size_t ServerConfig::write_queue_max() const {
  return static_cast<size_t>(CFG_INT(generic_, execution.write_queue_max));
}

size_t ServerConfig::max_result_rows() const {
  return static_cast<size_t>(CFG_INT(generic_, execution.max_result_rows));
}

int64_t ServerConfig::statement_timeout_ms() const {
  return CFG_INT(generic_, execution.statement_timeout_ms);
}

std::string ServerConfig::default_database() const {
  return CFG_STR(generic_, session.default_database);
}

bool ServerConfig::raft_enabled() const {
  return CFG_BOOL(generic_, raft.enabled);
}

uint64_t ServerConfig::raft_node_id() const {
  return static_cast<uint64_t>(CFG_INT(generic_, raft.node_id));
}

std::string ServerConfig::raft_listen() const {
  return CFG_STR(generic_, raft.listen);
}

std::string ServerConfig::raft_listen_host() const {
  const std::string listen = CFG_STR(generic_, raft.listen);
  const size_t colon = listen.rfind(':');
  return colon == std::string::npos ? std::string() : listen.substr(0, colon);
}

std::string ServerConfig::raft_listen_port() const {
  const std::string listen = CFG_STR(generic_, raft.listen);
  const size_t colon = listen.rfind(':');
  return colon == std::string::npos ? std::string() : listen.substr(colon + 1);
}

std::vector<raft::PeerConfig> ServerConfig::raft_peers() const {
  const std::string text = CFG_STR(generic_, raft.peers);
  if (text.empty()) {
    return {};
  }
  auto parsed = raft::parse_peer_list(text);
  if (!parsed.has_value()) {
    // validate() 已经拦过；真到这里说明调用方跳过了校验，报空而不是崩。
    return {};
  }
  return std::move(*parsed);
}

uint64_t ServerConfig::raft_election_timeout_ms() const {
  return static_cast<uint64_t>(CFG_INT(generic_, raft.election_timeout_ms));
}

uint64_t ServerConfig::raft_heartbeat_ms() const {
  return static_cast<uint64_t>(CFG_INT(generic_, raft.heartbeat_ms));
}

uint64_t ServerConfig::raft_snapshot_entries() const {
  return static_cast<uint64_t>(CFG_INT(generic_, raft.snapshot_entries));
}

std::string ServerConfig::raft_log_path() const {
  return CFG_STR(generic_, raft.log_path);
}

std::map<uint64_t, std::string> ServerConfig::raft_sql_endpoints() const {
  const std::string text = CFG_STR(generic_, raft.sql_endpoints);
  std::map<uint64_t, std::string> endpoints;
  if (text.empty()) {
    return endpoints;
  }
  auto parsed = raft::parse_peer_list(text);
  if (!parsed.has_value()) {
    return endpoints; // validate() 已经拦过；这里不抛，返回空 map
  }
  for (const auto &endpoint : *parsed) {
    endpoints[endpoint.node_id.value] =
        endpoint.host + ":" + std::to_string(endpoint.port);
  }
  return endpoints;
}

std::expected<ServerConfig, std::string> parse_config(const std::string &text) {
  auto overlay = parse_ini(text);
  if (!overlay.has_value()) {
    return std::unexpected(overlay.error());
  }
  Config config; // 默认值已带上
  config.merge(*overlay);
  return ServerConfig(std::move(config));
}

std::expected<ServerConfig, std::string> load_config(const std::string &path) {
  std::ifstream file(path);
  if (!file) {
    return std::unexpected("cannot open config file: " + path);
  }
  std::ostringstream buffer;
  buffer << file.rdbuf();
  auto parsed = parse_config(buffer.str());
  if (!parsed.has_value()) {
    return std::unexpected(path + ": " + parsed.error());
  }
  return parsed;
}

} // namespace server

#undef CFG_STR
#undef CFG_INT
#undef CFG_BOOL
