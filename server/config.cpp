// server/config.cpp
#include "config.h"

#include <cctype>
#include <fstream>
#include <sstream>
#include <unordered_map>

namespace server {
namespace {

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

bool parse_bool(const std::string &value, bool *out) {
  const std::string v = lower(value);
  if (v == "true" || v == "yes" || v == "1" || v == "on") {
    *out = true;
    return true;
  }
  if (v == "false" || v == "no" || v == "0" || v == "off") {
    *out = false;
    return true;
  }
  return false;
}

bool parse_int(const std::string &value, int64_t *out) {
  try {
    size_t used = 0;
    const long long parsed = std::stoll(value, &used, 10);
    if (used != value.size()) {
      return false;
    }
    *out = static_cast<int64_t>(parsed);
    return true;
  } catch (...) {
    return false;
  }
}

} // namespace

std::expected<Config, std::string> parse_config(const std::string &text) {
  Config config;
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
    const std::string value = trim(raw.substr(eq + 1));
    const std::string full = section.empty() ? key : section + "." + key;

    if (full == "server.listen") {
      config.listen = value;
    } else if (full == "server.max_connections") {
      int64_t parsed = 0;
      if (!parse_int(value, &parsed) || parsed <= 0) {
        return std::unexpected(fail(line_no, "max_connections must be > 0"));
      }
      config.max_connections = static_cast<size_t>(parsed);
    } else if (full == "server.idle_timeout_ms") {
      if (!parse_int(value, &config.idle_timeout_ms) ||
          config.idle_timeout_ms < 0) {
        return std::unexpected(fail(line_no, "idle_timeout_ms must be >= 0"));
      }
    } else if (full == "server.idle_in_transaction_timeout_ms") {
      if (!parse_int(value, &config.idle_in_transaction_timeout_ms) ||
          config.idle_in_transaction_timeout_ms < 0) {
        return std::unexpected(
            fail(line_no, "idle_in_transaction_timeout_ms must be >= 0"));
      }
    } else if (full == "server.log_level") {
      const std::string level = lower(value);
      if (level != "error" && level != "warn" && level != "info" &&
          level != "debug") {
        return std::unexpected(
            fail(line_no, "log_level must be error|warn|info|debug"));
      }
      config.log_level = level;
    } else if (full == "storage.engine") {
      const std::string engine = lower(value);
      if (engine != "leveldb" && engine != "mock") {
        return std::unexpected(fail(line_no, "engine must be leveldb|mock"));
      }
      config.engine = engine;
    } else if (full == "storage.path") {
      if (value.empty()) {
        return std::unexpected(fail(line_no, "path must not be empty"));
      }
      config.path = value;
    } else if (full == "storage.create_if_missing") {
      if (!parse_bool(value, &config.create_if_missing)) {
        return std::unexpected(fail(line_no, "expected a boolean"));
      }
    } else if (full == "execution.read_threads") {
      int64_t parsed = 0;
      if (!parse_int(value, &parsed) || parsed <= 0 || parsed > 64) {
        return std::unexpected(fail(line_no, "read_threads must be 1..64"));
      }
      config.read_threads = static_cast<size_t>(parsed);
    } else if (full == "execution.write_queue_max") {
      int64_t parsed = 0;
      if (!parse_int(value, &parsed) || parsed <= 0) {
        return std::unexpected(fail(line_no, "write_queue_max must be > 0"));
      }
      config.write_queue_max = static_cast<size_t>(parsed);
    } else if (full == "execution.max_result_rows") {
      int64_t parsed = 0;
      if (!parse_int(value, &parsed) || parsed <= 0) {
        return std::unexpected(fail(line_no, "max_result_rows must be > 0"));
      }
      config.max_result_rows = static_cast<size_t>(parsed);
    } else if (full == "execution.statement_timeout_ms") {
      if (!parse_int(value, &config.statement_timeout_ms) ||
          config.statement_timeout_ms < 0) {
        return std::unexpected(
            fail(line_no, "statement_timeout_ms must be >= 0"));
      }
    } else if (full == "session.default_database") {
      config.default_database = value;
    } else {
      // 未知键 = 拼错了：直接失败，别静默忽略
      return std::unexpected(fail(line_no, "unknown key: " + full));
    }
  }

  // 派生 host/port（listen = host:port）
  const size_t colon = config.listen.rfind(':');
  if (colon == std::string::npos || colon == 0 ||
      colon + 1 >= config.listen.size()) {
    return std::unexpected("listen must look like host:port");
  }
  config.host = config.listen.substr(0, colon);
  config.port = config.listen.substr(colon + 1);
  return config;
}

std::expected<Config, std::string> load_config(const std::string &path) {
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
