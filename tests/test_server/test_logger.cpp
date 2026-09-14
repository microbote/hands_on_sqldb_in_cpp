// tests/test_server/test_logger.cpp
//
// 日志：级别过滤 + 接收端（stderr / append 文件）。
//
// 这个文件**不起监听 socket**，所以沙箱里（bind 被禁）也能跑 —— 日志的正确性
// 本来就和网络无关。
#include "test_framework.h"

#include "server/logger.h"

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>
#include <unistd.h>
#include <vector>

namespace {

// 造一个还没被占用的路径（mkstemp 建完就删，留给 Logger 自己 O_CREAT）
std::string unique_path() {
  std::string templ = "/tmp/sqldb_logger_test_XXXXXX";
  std::vector<char> buffer(templ.begin(), templ.end());
  buffer.push_back('\0');
  const int fd = mkstemp(buffer.data());
  if (fd >= 0) {
    ::close(fd);
  }
  std::remove(buffer.data());
  return std::string(buffer.data());
}

std::string read_file(const std::string &path) {
  std::ifstream in(path, std::ios::binary);
  return std::string((std::istreambuf_iterator<char>(in)),
                     std::istreambuf_iterator<char>());
}

size_t count_lines(const std::string &text) {
  size_t count = 0;
  for (const char c : text) {
    if (c == '\n') {
      ++count;
    }
  }
  return count;
}

} // namespace

TEST(Logger, LevelGateKeepsOnlyMessagesAtOrBelowTheConfiguredLevel) {
  const std::string path = unique_path();
  auto logger = server::Logger::create("warn", path);
  CHECK(logger.has_value());
  if (!logger.has_value()) {
    return;
  }
  (*logger)->log(server::LogLevel::Debug, "debug-line");
  (*logger)->log(server::LogLevel::Info, "info-line");
  (*logger)->log(server::LogLevel::Warn, "warn-line");
  (*logger)->log(server::LogLevel::Error, "error-line");

  const std::string text = read_file(path);
  CHECK(text.find("debug-line") == std::string::npos);
  CHECK(text.find("info-line") == std::string::npos);
  CHECK(text.find("warn-line") != std::string::npos);
  CHECK(text.find("error-line") != std::string::npos);
  CHECK_EQ(count_lines(text), size_t{2});
  std::remove(path.c_str());
}

TEST(Logger, DebugLevelKeepsEverything) {
  const std::string path = unique_path();
  auto logger = server::Logger::create("debug", path);
  CHECK(logger.has_value());
  if (!logger.has_value()) {
    return;
  }
  (*logger)->log(server::LogLevel::Debug, "d");
  (*logger)->log(server::LogLevel::Info, "i");
  (*logger)->log(server::LogLevel::Warn, "w");
  (*logger)->log(server::LogLevel::Error, "e");

  const std::string text = read_file(path);
  CHECK_EQ(count_lines(text), size_t{4});
  // 每条都带级别名
  CHECK(text.find("[debug] d") != std::string::npos);
  CHECK(text.find("[error] e") != std::string::npos);
  std::remove(path.c_str());
}

TEST(Logger, LineCarriesTimestampAndLevel) {
  const std::string path = unique_path();
  auto logger = server::Logger::create("info", path);
  CHECK(logger.has_value());
  if (!logger.has_value()) {
    return;
  }
  (*logger)->log("info", "hello world");

  const std::string text = read_file(path);
  // 形如 [2026-09-14 12:34:56.789] [info] hello world\n
  CHECK_EQ(text.front(), '[');
  CHECK(text.find("] [info] hello world\n") != std::string::npos);
  // 时间戳长度固定（"YYYY-MM-DD HH:MM:SS.mmm" = 23 字符）
  const size_t close = text.find(']');
  CHECK_EQ(close, size_t{24}); // '[' + 23 字符
  std::remove(path.c_str());
}

TEST(Logger, FileIsOpenedInAppendModeSoRunsDoNotLoseHistory) {
  const std::string path = unique_path();
  {
    auto first = server::Logger::create("info", path);
    CHECK(first.has_value());
    if (first.has_value()) {
      (*first)->log("info", "first-run");
    }
  }
  {
    // 第二个实例再打开同一个文件：必须是追加，不能把上次的清掉
    auto second = server::Logger::create("info", path);
    CHECK(second.has_value());
    if (second.has_value()) {
      (*second)->log("info", "second-run");
    }
  }
  const std::string text = read_file(path);
  CHECK(text.find("first-run") != std::string::npos);
  CHECK(text.find("second-run") != std::string::npos);
  CHECK_EQ(count_lines(text), size_t{2});
  std::remove(path.c_str());
}

TEST(Logger, UnknownLevelIsRejectedAtStartup) {
  const auto logger = server::Logger::create("loud", "");
  CHECK(!logger.has_value());
  if (!logger.has_value()) {
    CHECK(logger.error().find("loud") != std::string::npos);
  }
}

TEST(Logger, UnopenableFileIsAnErrorNotEmptySilence) {
  // 目录不存在：启动期就该报错，而不是"以为在记日志"
  const auto logger = server::Logger::create("info", "/no/such/dir/x.log");
  CHECK(!logger.has_value());
  if (!logger.has_value()) {
    CHECK(logger.error().find("/no/such/dir/x.log") != std::string::npos);
  }
}

TEST(Logger, EmptyFileMeansStderr) {
  auto logger = server::Logger::create("info", "");
  CHECK(logger.has_value());
  if (!logger.has_value()) {
    return;
  }
  CHECK((*logger)->file().empty());
  CHECK((*logger)->enabled(server::LogLevel::Info));
  CHECK(!(*logger)->enabled(server::LogLevel::Debug));
}
