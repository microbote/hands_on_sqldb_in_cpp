// tests/test_server/test_config.cpp
//
// 配置 = 通用键值包（Config：map<section, map<key, sql::Value>>）
//      + 类型化门面（ServerConfig：每个字段一个方法）+ validate()。
// 分工：语法错 → parse_config 失败（带行号）；
//       字段错（拼写/类型/范围/枚举）→ validate() 失败（带字段名）。
#include "test_framework.h"

#include "server/config.h"

#include <string>

namespace {

// 语法 + 字段一起过（"这份配置能不能起服务"）
bool parses_and_validates(const std::string &text) {
  auto config = server::parse_config(text);
  return config.has_value() && config->validate().has_value();
}

} // namespace

TEST(Config, DefaultsAreUsable) {
  auto config = server::parse_config("");
  CHECK(config.has_value());
  if (!config.has_value()) {
    return;
  }
  CHECK(config->validate().has_value());
  CHECK_EQ(config->listen(), std::string("127.0.0.1:5433"));
  CHECK_EQ(config->listen_host(), std::string("127.0.0.1"));
  CHECK_EQ(config->listen_port(), std::string("5433"));
  CHECK_EQ(config->engine(), std::string("leveldb"));
  CHECK_EQ(config->read_threads(), size_t{1});
  CHECK_EQ(config->max_connections(), size_t{256});
  CHECK(config->create_if_missing());
}

TEST(Config, ParsesSectionsAndValues) {
  const std::string text = R"(
# 注释
[server]
listen = 0.0.0.0:9999
max_connections = 32
idle_timeout_ms = 0

[storage]
engine = mock
path = ./tmp_db
create_if_missing = false

[execution]
read_threads = 4
write_queue_max = 8
max_result_rows = 100
statement_timeout_ms = 1500

[session]
default_database = shop
)";
  auto config = server::parse_config(text);
  CHECK(config.has_value());
  if (!config.has_value()) {
    return;
  }
  CHECK(config->validate().has_value());
  CHECK_EQ(config->listen_host(), std::string("0.0.0.0"));
  CHECK_EQ(config->listen_port(), std::string("9999"));
  CHECK_EQ(config->max_connections(), size_t{32});
  CHECK_EQ(config->idle_timeout_ms(), int64_t{0});
  CHECK_EQ(config->engine(), std::string("mock"));
  CHECK_EQ(config->path(), std::string("./tmp_db"));
  CHECK(!config->create_if_missing());
  CHECK_EQ(config->read_threads(), size_t{4});
  CHECK_EQ(config->write_queue_max(), size_t{8});
  CHECK_EQ(config->max_result_rows(), size_t{100});
  CHECK_EQ(config->statement_timeout_ms(), int64_t{1500});
  CHECK_EQ(config->default_database(), std::string("shop"));
}

TEST(Config, ValuesAreSniffedIntoTypedValues) {
  auto config = server::parse_config(
      "[storage]\ncreate_if_missing = off\npath = ./tmp_db\n"
      "[execution]\nread_threads = 4\n");
  CHECK(config.has_value());
  if (!config.has_value()) {
    return;
  }
  // find 在通用包上（ServerConfig::generic() 是逃生舱）
  const sql::Value *flag = config->generic().find("storage.create_if_missing");
  CHECK(flag != nullptr);
  CHECK(flag->is_bool());
  const sql::Value *threads = config->generic().find("execution.read_threads");
  CHECK(threads != nullptr);
  CHECK(threads->is_int());
  const sql::Value *path = config->generic().find("storage.path");
  CHECK(path != nullptr);
  CHECK(path->is_string());
}

TEST(Config, SetOverridesDefaultsAndGetterFallbackWorks) {
  server::ServerConfig config; // 默认值已带上
  CHECK_EQ(config.engine(), std::string("leveldb"));
  config.set("storage.engine", "mock");
  CHECK_EQ(config.engine(), std::string("mock"));
  // 通用包：没见过的键走 getter 的 fallback
  const server::Config &bag = config.generic();
  CHECK(!bag.contains("no.such_key"));
  CHECK_EQ(bag.get_int("no.such_key", 42), int64_t{42});
  CHECK_EQ(bag.get_string("no.such_key", "x"), std::string("x"));
}

TEST(Config, UnknownKeyInKnownSectionFailsValidation) {
  // "lissten" 是拼错：语法没问题，validate 要拦下来
  auto config = server::parse_config("[server]\nlissten = 1.2.3.4:1\n");
  CHECK(config.has_value());
  if (!config.has_value()) {
    return;
  }
  auto ok = config->validate();
  CHECK(!ok.has_value());
  if (!ok.has_value()) {
    CHECK(ok.error().find("server.lissten") != std::string::npos);
  }
}

TEST(Config, UnknownSectionsAreComponentPrivateAndPass) {
  // 别的组件的自留地：不在默认值表里的 section 不做键名检查
  CHECK(parses_and_validates("[my_component]\nwhatever = 1\n"));
}

TEST(Config, BadValuesFailValidation) {
  CHECK(!parses_and_validates("[server]\nmax_connections = 0\n"));
  CHECK(!parses_and_validates("[server]\nmax_connections = abc\n"));
  CHECK(!parses_and_validates("[execution]\nread_threads = 999\n"));
  CHECK(!parses_and_validates("[storage]\nengine = mysql\n"));
  CHECK(!parses_and_validates("[server]\nlog_level = loud\n"));
  CHECK(!parses_and_validates("[storage]\npath =\n"));
  CHECK(!parses_and_validates("[server]\nlisten = nocolon\n"));
  CHECK(!parses_and_validates("[server]\nidle_timeout_ms = -1\n"));
}

TEST(Config, SyntaxErrorsFailWithLineNumber) {
  auto bad_section = server::parse_config("[server\nlisten = 1\n");
  CHECK(!bad_section.has_value());
  if (!bad_section.has_value()) {
    CHECK(bad_section.error().find("line 1") != std::string::npos);
  }
  auto no_equals = server::parse_config("[server]\nlisten 127.0.0.1:1\n");
  CHECK(!no_equals.has_value());
  if (!no_equals.has_value()) {
    CHECK(no_equals.error().find("line 2") != std::string::npos);
  }
}

TEST(Config, MissingFileIsReported) {
  auto config = server::load_config("/tmp/sqldb_no_such_config_file.conf");
  CHECK(!config.has_value());
  if (!config.has_value()) {
    CHECK(config.error().find("cannot open") != std::string::npos);
  }
}
