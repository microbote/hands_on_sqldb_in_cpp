// client/sqldb_client_main.cpp —— 远程客户端 `sqldb-client`
//
// 与本地 `sqldb` 共用同一套 REPL（client/repl.cpp），只把 transport 换成
// `client::RemoteConnection`（自定义协议，长连接、多语句交互）。
#include <cstdio>
#include <cstdlib>
#include <optional>
#include <string>
#include <unistd.h>

#include <fmt/format.h>

#include "client/connection.h"
#include "client/repl.h"

#if defined(SQLDB_HAVE_READLINE)
#include <readline/history.h>
#include <readline/readline.h>
#endif

namespace {

struct Options {
  std::string host = "127.0.0.1";
  std::string port = "5433";
  bool colors = true;
  bool echo_sql = false;
  bool force_interactive = false;
  std::string command; // -e
  bool loose_cross_group_reads = false;
};

void print_usage(const char *program) {
  fmt::print("Usage: {} [options]\n", program);
  fmt::print("  -e, --execute SQL   执行一条 SQL 后退出\n");
  fmt::print("  -i, --interactive   强制进入交互模式\n");
  fmt::print("      --host HOST     服务器地址（默认 127.0.0.1）\n");
  fmt::print("      --port PORT     服务器端口（默认 5433）\n");
  fmt::print("      --echo-sql      执行前回显\n");
  fmt::print("      --cross-group-read=MODE 事务跨组只读模式：strict（默认）|\n");
  fmt::print("                             loose（允许读多个组，事务冻结为只读）\n");
  fmt::print("      --no-color      关闭颜色\n");
  fmt::print("  -h, --help          显示帮助\n");
}

bool parse_options(int argc, char **argv, Options *options) {
  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    const auto value_of = [&](const std::string &prefix) {
      return arg.substr(prefix.size());
    };
    // 带值的选项同时支持 "--name value" 与 "--name=value" —— usage 里写的是
    // 空格形式，只认 '=' 的话用户照文档敲会得到"未知选项"。
    const auto take_value = [&](const char *name, std::string *out) -> bool {
      const std::string with_eq = std::string(name) + "=";
      if (arg == name) {
        if (i + 1 >= argc) {
          fmt::print(stderr, "{} 需要一个参数\n", name);
          return false;
        }
        *out = argv[++i];
        return true;
      }
      if (arg.starts_with(with_eq)) {
        *out = arg.substr(with_eq.size());
        return true;
      }
      return false;
    };
    if (arg == "-h" || arg == "--help") {
      print_usage(argv[0]);
      std::exit(0);
    } else if (arg == "-i" || arg == "--interactive") {
      options->force_interactive = true;
    } else if (arg == "--echo-sql") {
      options->echo_sql = true;
    } else if (arg == "--no-color") {
      options->colors = false;
    } else if (arg == "--color") {
      options->colors = true;
    } else if (arg == "-e" || arg == "--execute") {
      if (i + 1 >= argc) {
        fmt::print(stderr, "--execute 需要一个参数\n");
        return false;
      }
      options->command = argv[++i];
    } else if (arg.starts_with("--execute=")) {
      options->command = value_of("--execute=");
    } else if (arg == "--host" || arg.starts_with("--host=")) {
      if (!take_value("--host", &options->host)) {
        return false;
      }
    } else if (arg == "--port" || arg.starts_with("--port=")) {
      if (!take_value("--port", &options->port)) {
        return false;
      }
    } else if (arg == "--cross-group-read" ||
               arg.starts_with("--cross-group-read=")) {
      std::string mode;
      if (!take_value("--cross-group-read", &mode)) {
        return false;
      }
      if (mode == "loose") {
        options->loose_cross_group_reads = true;
      } else if (mode == "strict") {
        options->loose_cross_group_reads = false;
      } else {
        fmt::print(stderr, "--cross-group-read 只接受 loose 或 strict，得到：{}\n",
                   mode);
        return false;
      }
    } else {
      fmt::print(stderr, "未知选项：{}\n", arg);
      return false;
    }
  }
  return true;
}

bool is_interactive_terminal() {
#if defined(_WIN32)
  return false;
#else
  return isatty(fileno(stdin)) != 0;
#endif
}

bool is_stdout_terminal() {
#if defined(_WIN32)
  return false;
#else
  return isatty(fileno(stdout)) != 0;
#endif
}

// readline 的历史文件（没有 readline 时由 client 层 no-op）
std::string history_path() {
  const char *home = std::getenv("HOME");
  if (home == nullptr || *home == '\0') {
    return {};
  }
  return std::string(home) + "/.sqldb_client_history";
}

} // namespace

int main(int argc, char **argv) {
  Options options;
  if (!parse_options(argc, argv, &options)) {
    print_usage(argv[0]);
    return 2;
  }
  options.colors = options.colors && is_stdout_terminal();

  client::RemoteOptions remote;
  remote.host = options.host;
  remote.port = options.port;
  remote.loose_cross_group_reads = options.loose_cross_group_reads;
  std::string error;
  auto connection = client::make_remote(remote, &error);
  if (connection == nullptr) {
    fmt::print(stderr, "连接失败：{}\n", error);
    return 1;
  }

  client::ReplOptions repl_options;
  repl_options.colors = options.colors;
  repl_options.echo_sql = options.echo_sql;
  repl_options.interactive = true; // 远程模式默认是交互（或 -e 一条）

  int failures = 0;
  if (!options.command.empty()) {
    failures += client::run_text(*connection, options.command, repl_options);
    return failures == 0 ? 0 : 1;
  }
  if (options.force_interactive || is_interactive_terminal()) {
    const std::string history = history_path();
    auto reader = client::readline_line_reader(connection.get(), history);
    failures += client::run(*connection, repl_options, reader);
    client::history_flush(history);
    return failures == 0 ? 0 : 1;
  }
  // 管道输入：当脚本跑
  std::string content;
  char buffer[4096];
  size_t read = 0;
  while ((read = std::fread(buffer, 1, sizeof(buffer), stdin)) > 0) {
    content.append(buffer, read);
  }
  failures += client::run_text(*connection, content, repl_options);
  return failures == 0 ? 0 : 1;
}
