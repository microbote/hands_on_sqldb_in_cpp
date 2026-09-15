// cmdline/main.cpp —— 本地客户端 `sqldb`
//
// 只做四件事：解析命令行选项 → 打开本地存储（一份 KVStore + 一条连接）→
// 包成 `client::LocalConnection` → 交给**共用 REPL**（client/repl.cpp）。
//
// 远程客户端 `sqldb-client`（client/sqldb_client_main.cpp）走同一个 REPL，
// 只是把 transport 换成 `client::RemoteConnection`（自定义协议）。
#include <cstdio>
#include <cstdlib>
#include <optional>
#include <string>
#include <unistd.h>
#include <utility>
#include <vector>

#include <fmt/format.h>

#include "client/connection.h"
#include "client/repl.h"
#include "storage/kv_engine/kv_factory.h"

#if defined(SQLDB_HAVE_READLINE)
#include <readline/history.h>
#include <readline/readline.h>
#endif

namespace {

// ============================================================
// 命令行选项
// ============================================================
struct Options {
  std::string engine = "default"; // default/mock/leveldb
  bool loose_cross_group_reads = false;
  std::string path = "./sql_db";
  bool force_interactive = false;
  bool echo_sql = false;
  bool colors = true;  // 默认跟着 stdout 是不是 TTY（见 main）
  std::string command; // -e
  std::vector<std::string> scripts;
};

void print_usage(const char *program) {
  fmt::print("Usage: {} [options] [script.sql ...]\n", program);
  fmt::print("\nOptions:\n");
  fmt::print("  -e, --execute SQL   执行一条 SQL 后退出\n");
  fmt::print("  -i, --interactive   强制进入交互模式\n");
  fmt::print("      --engine NAME   mock | leveldb（默认 leveldb，没有就退回 "
             "mock）\n");
  fmt::print("      --path DIR      leveldb 数据目录（默认 ./sql_db）\n");
  fmt::print("      --echo-sql      执行前回显（带语法高亮）\n");
  fmt::print("      --cross-group-read=MODE 事务跨组只读模式：strict（默认）|\n");
  fmt::print("                             loose（允许读多个组，事务冻结为只读）\n");
  fmt::print("      --no-color      关闭颜色\n");
  fmt::print("  -h, --help          显示帮助\n");
  fmt::print("\n交互模式内置命令：exit / quit / \\q 退出\n");
  fmt::print("另外支持 EXPLAIN [ANALYZE] <SELECT|INSERT|UPDATE|DELETE>"
             "（只出计划；带 ANALYZE 会真跑一遍 SELECT 并报实际行数/耗时）\n");
  fmt::print("元命令：\\l 列库  \\dt 列表  \\d <表> 看结构  \\c <库> 切换  \\? "
             "帮助\n");
  fmt::print("连远程服务器请用 sqldb-client --host=... --port=...\n");
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
    } else if (arg == "--engine" || arg.starts_with("--engine=")) {
      if (!take_value("--engine", &options->engine)) {
        return false;
      }
    } else if (arg == "--path" || arg.starts_with("--path=")) {
      if (!take_value("--path", &options->path)) {
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
    } else if (!arg.empty() && arg[0] == '-') {
      fmt::print(stderr, "未知选项：{}\n", arg);
      return false;
    } else {
      options->scripts.push_back(arg);
    }
  }
  return true;
}

std::shared_ptr<kv::KVEngine> create_engine(const Options &options) {
  const bool want_leveldb =
      options.engine == "leveldb" || options.engine == "default";
#if defined(SQLDB_HAVE_LEVELDB)
  if (want_leveldb) {
    kv::DatabaseOptions db_options;
    db_options.set_path(options.path).set_create_if_missing(true);
    auto store = kv::open_store(kv::EngineType::LEVELDB, db_options);
    if (store != nullptr) {
      return store->connect(); // CLI 一条连接；服务器上这里按客户端再连
    }
    fmt::print(stderr, "打开 leveldb({}) 失败，退回内存引擎\n", options.path);
  }
#else
  if (options.engine == "leveldb") {
    fmt::print(stderr, "这个构建没有编入 leveldb，改用内存引擎\n");
  }
#endif
  kv::DatabaseOptions db_options;
  db_options.set_path("mock://sqldb");
  auto store = kv::open_store(kv::EngineType::MOCK, db_options);
  return store != nullptr ? store->connect() : nullptr;
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
  return std::string(home) + "/.sqldb_history";
}

} // namespace

int main(int argc, char **argv) {
  Options options;
  if (!parse_options(argc, argv, &options)) {
    print_usage(argv[0]);
    return 2;
  }
  // 输出重定向/管道时默认不上色（脚本输出干净）；--color 可以强制打开
  options.colors = options.colors && is_stdout_terminal();
  auto engine = create_engine(options);
  if (engine == nullptr || !engine->is_open()) {
    fmt::print(stderr, "存储引擎没有打开\n");
    return 1;
  }
  engine->set_loose_cross_group_reads(options.loose_cross_group_reads);

  client::ReplOptions repl_options;
  repl_options.colors = options.colors;
  repl_options.echo_sql = options.echo_sql;
  auto connection = client::make_local(std::move(engine));

  int failures = 0;
  if (!options.command.empty()) {
    failures += client::run_text(*connection, options.command, repl_options);
  }
  for (const std::string &script : options.scripts) {
    FILE *file = std::fopen(script.c_str(), "rb");
    if (file == nullptr) {
      fmt::print(stderr, "打不开脚本：{}\n", script);
      failures += 1;
      continue;
    }
    std::string content;
    char buffer[4096];
    size_t read = 0;
    while ((read = std::fread(buffer, 1, sizeof(buffer), file)) > 0) {
      content.append(buffer, read);
    }
    std::fclose(file);
    failures += client::run_text(*connection, content, repl_options);
  }

  const bool interactive =
      options.force_interactive ||
      (options.command.empty() && options.scripts.empty() &&
       is_interactive_terminal());
  if (interactive) {
    repl_options.interactive = true;
    const std::string history = history_path();
    auto reader = client::readline_line_reader(connection.get(), history);
    failures += client::run(*connection, repl_options, reader);
    client::history_flush(history);
  } else if (options.command.empty() && options.scripts.empty()) {
    // 非交互（管道输入）：把 stdin 当成脚本
    std::string content;
    char buffer[4096];
    size_t read = 0;
    while ((read = std::fread(buffer, 1, sizeof(buffer), stdin)) > 0) {
      content.append(buffer, read);
    }
    failures += client::run_text(*connection, content, repl_options);
  }
  return failures == 0 ? 0 : 1;
}
