// cmdline/main.cpp
//
// sqldb：把 SQL 交给 session::Session 执行的命令行客户端。
//
//   交互模式：  ./sqldb                       （默认读 TTY，支持多行语句到 ';'
//   为止） 单条执行：  ./sqldb -e "SELECT * FROM users" 跑脚本：    ./sqldb
//   script.sql [more.sql]
//
// 分层：CLI 只做"读入 -> 交给 session -> 打印"，不碰 parser/planner/executor
// 的细节；错误由 SessionError 带出来（信息 + 位置 + 高亮片段）。
#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

#if !defined(_WIN32)
#include <unistd.h> // isatty / fileno
#endif

#include <fmt/color.h>
#include <fmt/format.h>

#if defined(SQLDB_HAVE_READLINE)
#include <readline/history.h>
#include <readline/readline.h>
#endif

#include "executor/executor.h"
#include "session/session.h"
#include "sql_types/row.h"
#include "statement/sql_highlight.h"
#include "storage/kv_engine/kv_engine.h"
#include "storage/mock_engine/mock_engine.h"

#if defined(SQLDB_HAVE_LEVELDB)
#include "storage/kv_engine/kv_factory.h"
#endif

namespace {

// ============================================================
// 命令行选项
// ============================================================
struct Options {
  std::string engine = "default"; // default/mock/leveldb
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
  fmt::print("      --no-color      关闭颜色\n");
  fmt::print("  -h, --help          显示帮助\n");
  fmt::print("\n交互模式内置命令：exit / quit / \\q 退出\n");
  fmt::print("另外支持 EXPLAIN [ANALYZE] <SELECT|INSERT|UPDATE|DELETE>"
             "（只出计划；带 ANALYZE 会真跑一遍 SELECT 并报实际行数/耗时）\n");
  fmt::print("元命令：\\l 列库  \\dt 列表  \\d <表> 看结构  \\c <库> 切换  \\? "
             "帮助\n");
}

bool parse_options(int argc, char **argv, Options *options) {
  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    const auto value_of = [&](const std::string &prefix) {
      return arg.substr(prefix.size());
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
    } else if (arg.starts_with("--engine=")) {
      options->engine = value_of("--engine=");
    } else if (arg.starts_with("--path=")) {
      options->path = value_of("--path=");
    } else if (!arg.empty() && arg[0] == '-') {
      fmt::print(stderr, "未知选项：{}\n", arg);
      return false;
    } else {
      options->scripts.push_back(arg);
    }
  }
  return true;
}

// ============================================================
// 语句切分：按顶层 ';' 切开（跳过引号里的分号）
//
// 记下每个语句在原文里的起始位置：session/parser 看到的是"单条语句"，
// 行号是块内相对的；打印前用起始位置换算成文件绝对位置（脚本模式尤其需要）。
// ============================================================
struct Statement {
  std::string text;
  uint32_t start_line = 1;
  uint32_t start_column = 1;
};

std::vector<Statement> split_statements(const std::string &text) {
  std::vector<Statement> statements;
  std::string current;
  uint32_t line = 1;
  uint32_t column = 1;
  uint32_t start_line = 1;
  uint32_t start_column = 1;
  char quote = '\0';
  for (const char c : text) {
    if (current.empty() && c != ';') { // 记住这条语句的起点
      start_line = line;
      start_column = column;
    }
    if (c == '\n') {
      ++line;
      column = 1;
    } else {
      ++column;
    }
    if (quote != '\0') {
      current.push_back(c);
      if (c == quote) {
        quote = '\0';
      }
      continue;
    }
    if (c == '\'' || c == '"') {
      quote = c;
      current.push_back(c);
      continue;
    }
    if (c == ';') {
      if (!current.empty()) {
        statements.push_back(Statement{current, start_line, start_column});
        current.clear();
      }
      start_line = line;
      start_column = column;
      continue;
    }
    current.push_back(c);
  }
  if (!current.empty()) { // 结尾没写分号也算一条
    statements.push_back(Statement{current, start_line, start_column});
  }
  return statements;
}

// 把 session 报的"块内位置"换算成脚本里的绝对位置
session::SessionError to_absolute(session::SessionError error,
                                  const Statement &statement) {
  if (!sspan_valid(error.span)) {
    return error;
  }
  if (error.span.begin_line == 1) {
    error.span.begin_column += statement.start_column - 1;
  }
  if (error.span.end_line == 1) {
    error.span.end_column += statement.start_column - 1;
  }
  error.span.begin_line += statement.start_line - 1;
  error.span.end_line += statement.start_line - 1;
  return error;
}

bool is_exit_command(const std::string &line) {
  return line == "exit" || line == "quit" || line == "\\q" || line == "EXIT" ||
         line == "QUIT";
}

// 这条语句是不是 EXPLAIN（独立关键字，大小写不敏感）
bool is_explain(const std::string &statement) {
  size_t begin = 0;
  while (begin < statement.size() &&
         std::isspace(static_cast<unsigned char>(statement[begin])) != 0) {
    ++begin;
  }
  constexpr size_t kLength = 7; // "explain"
  if (statement.size() < begin + kLength) {
    return false;
  }
  std::string head = statement.substr(begin, kLength);
  for (char &c : head) {
    c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  }
  if (head != "explain") {
    return false;
  }
  const size_t after = begin + kLength;
  return after >= statement.size() ||
         std::isspace(static_cast<unsigned char>(statement[after])) != 0;
}

// ============================================================
// 打印
// ============================================================
void print_error(const session::SessionError &error, bool colors) {
  const auto style = colors ? fmt::fg(fmt::color::red) | fmt::emphasis::bold
                            : fmt::text_style{};
  // 先冲 stdout：否则管道/重定向下 stdout 还是缓冲的，错误会"抢跑"到前面
  std::fflush(stdout);
  fmt::print(stderr, style, "{}\n", error.to_string());
  const std::string highlight = error.highlight(colors);
  if (!highlight.empty()) {
    fmt::print(stderr, "{}\n", highlight);
  }
}

// 元命令的错误/提示：同样先冲 stdout，保证管道下顺序正确
void print_notice(const std::string &text) {
  std::fflush(stdout);
  fmt::print(stderr, "{}\n", text);
}

// 结果表格：列宽按内容算（按字节数；CJK 会略微不齐，够用）
// 通用表格打印（元命令与 SELECT 结果都用它）
void print_grid(const std::vector<std::string> &headers,
                const std::vector<std::vector<std::string>> &rows,
                bool colors) {
  std::vector<size_t> widths(headers.size(), 0);
  for (size_t i = 0; i < headers.size(); ++i) {
    widths[i] = headers[i].size();
  }
  for (const auto &row : rows) {
    for (size_t i = 0; i < headers.size() && i < row.size(); ++i) {
      widths[i] = std::max(widths[i], row[i].size());
    }
  }

  std::string header;
  for (size_t i = 0; i < headers.size(); ++i) {
    header += fmt::format("{:<{}}", headers[i], widths[i]);
    header += (i + 1 == headers.size()) ? "" : "  ";
  }
  fmt::print(colors ? fmt::emphasis::bold : fmt::text_style{}, "{}\n", header);

  std::string rule;
  for (size_t i = 0; i < headers.size(); ++i) {
    rule += std::string(widths[i], '-');
    rule += (i + 1 == headers.size()) ? "" : "  ";
  }
  fmt::print("{}\n", rule);

  for (const auto &row : rows) {
    std::string text;
    for (size_t i = 0; i < headers.size(); ++i) {
      text += fmt::format("{:<{}}", i < row.size() ? row[i] : std::string(),
                          widths[i]);
      text += (i + 1 == headers.size()) ? "" : "  ";
    }
    fmt::print("{}\n", text);
  }
}

void print_rows(const exec::ResultCursor &cursor,
                const std::vector<sql::Row> &rows, bool colors) {
  const std::vector<std::string> &columns = cursor.columns();
  std::vector<std::vector<std::string>> rendered;
  rendered.reserve(rows.size());
  for (const sql::Row &row : rows) {
    std::vector<std::string> line;
    line.reserve(columns.size());
    for (size_t i = 0; i < columns.size(); ++i) {
      line.push_back(i < row.size() ? row[i].to_string() : std::string());
    }
    rendered.push_back(std::move(line));
  }
  print_grid(columns, rendered, colors);
  fmt::print("({} row{})\n", rows.size(), rows.size() == 1 ? "" : "s");
}

// ============================================================
// 执行
// ============================================================
// 返回值：0 成功，1 失败
// 语句已经执行成功：打印结果（0 成功，1 失败——取行过程中可能出错）
int run_cursor(exec::ResultCursor &cursor, const Options &options) {
  if (!cursor.root()->produces_rows()) {
    // 写语句报受影响行数；DDL/USE 只说 OK
    if (cursor.error().is_error()) {
      fmt::print(stderr, "{}\n", cursor.error().to_string());
      return 1;
    }
    if (!cursor.root()->is_write()) {
      fmt::print("OK\n");
      return 0;
    }
    const size_t affected = cursor.affected_rows();
    fmt::print("OK, {} row{} affected\n", affected, affected == 1 ? "" : "s");
    return 0;
  }

  std::vector<sql::Row> rows;
  sql::CursorError error;
  while (true) {
    auto row = cursor.next();
    if (row.has_value()) {
      rows.push_back(std::move(*row));
      continue;
    }
    error = row.error();
    break;
  }
  if (error.is_error()) {
    fmt::print(stderr, "{}\n", error.to_string());
    return 1;
  }
  print_rows(cursor, rows, options.colors);
  return 0;
}

// ============================================================
// 元命令：\l \dt \d [table] \c <db> \?
// ============================================================
bool is_meta_command(const std::string &line) {
  size_t begin = 0;
  while (begin < line.size() &&
         std::isspace(static_cast<unsigned char>(line[begin])) != 0) {
    ++begin;
  }
  return begin < line.size() && line[begin] == '\\';
}

// 切成 (命令, 参数)；参数已去掉首尾空白
std::pair<std::string, std::string> split_meta_line(const std::string &line) {
  size_t begin = 0;
  while (begin < line.size() &&
         std::isspace(static_cast<unsigned char>(line[begin])) != 0) {
    ++begin;
  }
  size_t end = begin;
  while (end < line.size() &&
         std::isspace(static_cast<unsigned char>(line[end])) == 0) {
    ++end;
  }
  std::string command = line.substr(begin, end - begin);
  size_t arg_begin = end;
  while (arg_begin < line.size() &&
         std::isspace(static_cast<unsigned char>(line[arg_begin])) != 0) {
    ++arg_begin;
  }
  size_t arg_end = line.size();
  while (arg_end > arg_begin &&
         std::isspace(static_cast<unsigned char>(line[arg_end - 1])) != 0) {
    --arg_end;
  }
  return {command, line.substr(arg_begin, arg_end - arg_begin)};
}

std::string format_time(int64_t unix_seconds) {
  if (unix_seconds <= 0) {
    return "-";
  }
  const std::time_t when = static_cast<std::time_t>(unix_seconds);
  std::tm parts{};
#if defined(_WIN32)
  localtime_s(&parts, &when);
#else
  localtime_r(&when, &parts);
#endif
  char buffer[32];
  std::strftime(buffer, sizeof(buffer), "%Y-%m-%d %H:%M:%S", &parts);
  return buffer;
}

void print_meta_help() {
  fmt::print("元命令：\n");
  fmt::print("  \\l                  列出所有数据库（含建库时间、表数量，* "
             "为当前库）\n");
  fmt::print("  \\dt                 "
             "列出当前库的表（行数/列数/主键/创建时间/最后写入）\n");
  fmt::print(
      "  \\d [表]             不带参数等于 \\dt；带参数看表结构与统计\n");
  fmt::print("  \\d [库.]表          指定库里的表\n");
  fmt::print("  \\c <库>             切换当前数据库（等价 USE <库>）\n");
  fmt::print("  \\?                  显示这份帮助\n");
  fmt::print("  exit / quit / \\q    退出\n");
  fmt::print("  EXPLAIN <语句>         只打印计划，不执行\n");
  fmt::print(
      "  EXPLAIN ANALYZE <查询> 真跑一遍 SELECT，报每个算子的行数与耗时\n");
}

int run_meta_command(session::Session &session, const std::string &line,
                     const Options &options) {
  const auto [command, argument] = split_meta_line(line);

  if (command == "\\?" || command == "\\h" || command == "\\help") {
    print_meta_help();
    return 0;
  }

  if (command == "\\l" || command == "\\list") {
    const std::vector<session::DatabaseInfo> databases = session.databases();
    if (databases.empty()) {
      fmt::print("(没有数据库，用 CREATE DATABASE 建一个)\n");
      return 0;
    }
    std::vector<std::vector<std::string>> rows;
    for (const auto &db : databases) {
      rows.push_back(
          {db.is_current ? "* " + db.name.str() : "  " + db.name.str(),
           std::to_string(db.table_count), format_time(db.created_at)});
    }
    print_grid({"Database", "Tables", "Created"}, rows, options.colors);
    return 0;
  }

  // \d [db.]table：结构 + 统计
  if (command == "\\d" && !argument.empty()) {
    sql::Identifier db;
    std::string table_name = argument;
    const size_t dot = argument.find('.');
    if (dot != std::string::npos) {
      db = sql::Identifier(argument.substr(0, dot));
      table_name = argument.substr(dot + 1);
    }
    const sql::Identifier target_db =
        db.empty() ? session.current_database() : db;
    if (target_db.empty()) {
      print_notice("当前没有选中数据库（用 USE <库> 或 \\c <库>）");
      return 1;
    }

    const sql::Identifier wanted(table_name);
    const std::vector<session::TableInfo> tables = session.tables(target_db);
    const session::TableInfo *info = nullptr;
    for (const auto &table : tables) {
      if (table.name == wanted) {
        info = &table;
      }
    }
    auto schema = session.table_schema(wanted, target_db);
    if (info == nullptr || !schema.has_value()) {
      print_notice("表不存在：" + target_db.str() + "." + table_name);
      return 1;
    }
    fmt::print("{}", schema->to_string_table());
    fmt::print("统计：rows={}  columns={}  primary key={}\n", info->row_count,
               info->column_count,
               info->primary_key.empty() ? "-" : info->primary_key.str());
    fmt::print("      created={}  last write={}\n",
               format_time(info->created_at), format_time(info->last_write_at));
    return 0;
  }

  if (command == "\\dt" || command == "\\d") {
    // \dt [db] / \d：列某个库（默认当前库）的表
    sql::Identifier target = argument.empty() ? session.current_database()
                                              : sql::Identifier(argument);
    if (target.empty()) {
      print_notice("当前没有选中数据库（用 USE <库> 或 \\c <库>）");
      return 1;
    }
    if (!session.catalog().database_exists(target)) {
      print_notice("数据库不存在：" + target.str());
      return 1;
    }
    const std::vector<session::TableInfo> tables = session.tables(target);
    if (tables.empty()) {
      fmt::print("(库 {} 没有表)\n", target.empty() ? "(none)" : target.str());
      return 0;
    }
    std::vector<std::vector<std::string>> rows;
    for (const auto &table : tables) {
      rows.push_back({table.name.str(), std::to_string(table.row_count),
                      std::to_string(table.column_count),
                      table.primary_key.empty() ? "-" : table.primary_key.str(),
                      format_time(table.created_at),
                      format_time(table.last_write_at)});
    }
    print_grid(
        {"Table", "Rows", "Columns", "Primary key", "Created", "Last write"},
        rows, options.colors);
    return 0;
  }

  if (command == "\\c" || command == "\\connect") {
    if (argument.empty()) {
      print_notice("用法：\\c <库名>");
      return 1;
    }
    auto result = session.execute("USE " + argument);
    if (!result.has_value()) {
      print_error(result.error(), options.colors);
      return 1;
    }
    fmt::print("现在连接的是数据库 \"{}\"\n", session.current_database().str());
    return 0;
  }

  print_notice("未知元命令：" + command + "（用 \\? 看帮助）");
  return 1;
}

// 跑一段文本（可能含多条语句与元命令行）
int run_text(session::Session &session, const std::string &text,
             const Options &options) {
  int failures = 0;
  std::string pending;
  uint32_t pending_start_line = 1;
  uint32_t pending_start_column = 1;

  const auto flush = [&]() {
    if (pending.empty()) {
      return;
    }
    // 这一段的起点：pending 的第一行/列（用于把错误位置换算成绝对位置）
    Statement chunk{pending, pending_start_line, pending_start_column};
    for (const Statement &statement : split_statements(chunk.text)) {
      if (statement.text.find_first_not_of(" \t\r\n") == std::string::npos) {
        continue;
      }
      Statement absolute{statement.text,
                         chunk.start_line + statement.start_line - 1,
                         statement.start_line == 1
                             ? chunk.start_column + statement.start_column - 1
                             : statement.start_column};
      if (options.echo_sql) {
        fmt::print("{}\n", stmt::highlight_sql(statement.text, options.colors));
      }
      // EXPLAIN：只出计划，不执行
      if (is_explain(statement.text)) {
        auto explained = session.explain(statement.text);
        if (!explained.has_value()) {
          print_error(to_absolute(explained.error(), absolute), options.colors);
          ++failures;
          continue;
        }
        fmt::print("{}", *explained);
        continue;
      }
      auto result = session.execute(statement.text);
      if (!result.has_value()) {
        print_error(to_absolute(result.error(), absolute), options.colors);
        ++failures;
        continue;
      }
      failures += run_cursor(**result, options);
    }
    pending.clear();
  };

  // 逐行处理：元命令（反斜杠开头）独占一行，不参与语句累积；
  // 其余累积到出现 ';' 为止（保持脚本行号与文件一致）
  uint32_t line_number = 1;
  size_t begin = 0;
  while (begin <= text.size()) {
    const size_t newline = text.find('\n', begin);
    const std::string line =
        text.substr(begin, newline == std::string::npos ? std::string::npos
                                                        : newline - begin);
    if (pending.empty() && is_meta_command(line)) {
      failures += run_meta_command(session, line, options);
    } else {
      if (pending.empty()) {
        pending_start_line = line_number;
        pending_start_column = 1;
      }
      pending += line;
      pending += "\n";
      if (line.find(';') != std::string::npos) {
        flush();
      }
    }
    if (newline == std::string::npos) {
      break;
    }
    begin = newline + 1;
    ++line_number;
  }
  flush(); // 结尾没有分号也算一条
  return failures;
}

// ============================================================
// 交互模式（支持多行，直到分号）
// ============================================================
int repl(session::Session &session, const Options &options) {
  const auto prompt = [&]() {
    const sql::Identifier db = session.current_database();
    const std::string name = db.empty() ? "(none)" : db.str();
    return options.colors ? fmt::format("\033[1;36m{}\033[0m> ", name)
                          : name + "> ";
  };

  fmt::print("sqldb 命令行（输入 exit / quit / \\q 退出）\n");
  std::string pending;
  while (true) {
    std::string line;
#if defined(SQLDB_HAVE_READLINE)
    char *raw = readline(pending.empty() ? prompt().c_str() : "   ... ");
    if (raw == nullptr) { // Ctrl-D
      fmt::print("\n");
      break;
    }
    line = raw;
    free(raw);
    if (!line.empty()) {
      add_history(line.c_str());
    }
#else
    fmt::print("{}", pending.empty() ? prompt() : std::string("   ... "));
    if (!std::getline(std::cin, line)) {
      fmt::print("\n");
      break;
    }
#endif
    if (pending.empty() && is_exit_command(line)) {
      break;
    }
    if (line.empty() && pending.empty()) {
      continue;
    }
    pending += line + "\n";
    if (line.find(';') == std::string::npos) {
      continue; // 语句还没写完，继续读
    }
    run_text(session, pending, options);
    pending.clear();
  }
  return 0;
}

// ============================================================
// 引擎与入口
// ============================================================
std::shared_ptr<kv::KVEngine> create_engine(const Options &options) {
  const bool want_leveldb =
      options.engine == "leveldb" || options.engine == "default";
#if defined(SQLDB_HAVE_LEVELDB)
  if (want_leveldb) {
    auto engine = kv::KVEngineFactory::create(kv::EngineType::LEVELDB);
    kv::DatabaseOptions db_options;
    db_options.set_path(options.path).set_create_if_missing(true);
    if (engine != nullptr &&
        engine->open_database(db_options) == kv::Status::OK) {
      return engine;
    }
    fmt::print(stderr, "打开 leveldb({}) 失败，退回内存引擎\n", options.path);
  }
#else
  if (options.engine == "leveldb") {
    fmt::print(stderr, "这个构建没有编入 leveldb，改用内存引擎\n");
  }
#endif
  auto engine = std::make_shared<kv::MockEngine>();
  kv::DatabaseOptions db_options;
  db_options.set_path("mock://sqldb");
  engine->open_database(db_options);
  return engine;
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
  session::Session session(engine);

  int failures = 0;
  if (!options.command.empty()) {
    failures += run_text(session, options.command, options);
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
    failures += run_text(session, content, options);
  }

  const bool interactive =
      options.force_interactive ||
      (options.command.empty() && options.scripts.empty() &&
       is_interactive_terminal());
  if (interactive) {
    failures += repl(session, options);
  } else if (options.command.empty() && options.scripts.empty()) {
    // 非交互（管道输入）：把 stdin 当成脚本
    std::string content;
    char buffer[4096];
    size_t read = 0;
    while ((read = std::fread(buffer, 1, sizeof(buffer), stdin)) > 0) {
      content.append(buffer, read);
    }
    failures += run_text(session, content, options);
  }
  return failures == 0 ? 0 : 1;
}
