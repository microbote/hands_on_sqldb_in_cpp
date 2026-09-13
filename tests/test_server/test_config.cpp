// tests/test_server/test_config.cpp
//
// 配置解析：正确值、派生 host/port、以及**任何可疑配置都要启动失败**
// （未知键 = 拼错了，不能静默用默认值）。
#include "test_framework.h"

#include "server/config.h"

#include <string>

TEST(Config, DefaultsAreUsable) {
  auto config = server::parse_config("");
  CHECK(config.has_value());
  if (!config.has_value()) {
    return;
  }
  CHECK_EQ(config->listen, std::string("127.0.0.1:5433"));
  CHECK_EQ(config->host, std::string("127.0.0.1"));
  CHECK_EQ(config->port, std::string("5433"));
  CHECK_EQ(config->engine, std::string("leveldb"));
  CHECK_EQ(config->read_threads, size_t{1});
  CHECK_EQ(config->max_connections, size_t{256});
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
    CHECK_EQ(config.error(), std::string("<unexpected config error>"));
    return;
  }
  CHECK_EQ(config->host, std::string("0.0.0.0"));
  CHECK_EQ(config->port, std::string("9999"));
  CHECK_EQ(config->max_connections, size_t{32});
  CHECK_EQ(config->idle_timeout_ms, int64_t{0});
  CHECK_EQ(config->engine, std::string("mock"));
  CHECK_EQ(config->path, std::string("./tmp_db"));
  CHECK(!config->create_if_missing);
  CHECK_EQ(config->read_threads, size_t{4});
  CHECK_EQ(config->write_queue_max, size_t{8});
  CHECK_EQ(config->max_result_rows, size_t{100});
  CHECK_EQ(config->statement_timeout_ms, int64_t{1500});
  CHECK_EQ(config->default_database, std::string("shop"));
}

TEST(Config, UnknownKeyFailsWithLineNumber) {
  auto config = server::parse_config("[server]\nlissten = 1.2.3.4:1\n");
  CHECK(!config.has_value());
  if (!config.has_value()) {
    CHECK(config.error().find("line 2") != std::string::npos);
    CHECK(config.error().find("lissten") != std::string::npos);
  }
}

TEST(Config, BadValuesFail) {
  CHECK(!server::parse_config("[server]\nmax_connections = 0\n").has_value());
  CHECK(!server::parse_config("[server]\nmax_connections = abc\n").has_value());
  CHECK(!server::parse_config("[execution]\nread_threads = 999\n").has_value());
  CHECK(!server::parse_config("[storage]\nengine = mysql\n").has_value());
  CHECK(!server::parse_config("[server]\nlog_level = loud\n").has_value());
  CHECK(!server::parse_config("[storage]\npath =\n").has_value());
  CHECK(!server::parse_config("[server]\nlisten = nocolon\n").has_value());
  CHECK(!server::parse_config("[server]\nidle_timeout_ms = -1\n").has_value());
}

TEST(Config, SectionHeaderAndAssignmentSyntaxAreChecked) {
  CHECK(!server::parse_config("[server\nlisten = 1\n").has_value());
  CHECK(!server::parse_config("[server]\nlisten 127.0.0.1:1\n").has_value());
}

TEST(Config, MissingFileIsReported) {
  auto config = server::load_config("/tmp/sqldb_no_such_config_file.conf");
  CHECK(!config.has_value());
  if (!config.has_value()) {
    CHECK(config.error().find("cannot open") != std::string::npos);
  }
}
