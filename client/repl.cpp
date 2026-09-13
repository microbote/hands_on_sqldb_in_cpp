// client/repl.cpp —— 从 cmdline/main.cpp 抽出来的共用交互层
#include "repl.h"

#include <algorithm>
#include <cstring>
#include <ctime>
#include <iostream>
#include <string>
#include <utility>
#include <vector>

#include <fmt/color.h>

#include "statement/sql_highlight.h"

namespace client {
namespace {

// ============================================================
// 语句切分（按顶层 ';'，跳过引号里的分号）+ 记住起点行列
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
    if (current.empty() && c != ';') {
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
  if (!current.empty()) {
    statements.push_back(Statement{current, start_line, start_column});
  }
  return statements;
}

// 把"块内位置"换算成脚本里的绝对位置
Outcome to_absolute(Outcome outcome, const Statement &statement) {
  if (!sspan_valid(outcome.error_span)) {
    return outcome;
  }
  if (outcome.error_span.begin_line == 1) {
    outcome.error_span.begin_column += statement.start_column - 1;
  }
  if (outcome.error_span.end_line == 1) {
    outcome.error_span.end_column += statement.start_column - 1;
  }
  outcome.error_span.begin_line += statement.start_line - 1;
  outcome.error_span.end_line += statement.start_line - 1;
  return outcome;
}

bool is_exit_command(const std::string &line) {
  return line == "exit" || line == "quit" || line == "\\q" || line == "EXIT" ||
         line == "QUIT";
}

// ============================================================
// 打印
// ============================================================
void print_error(FILE *err, const Outcome &outcome, bool colors) {
  const auto style = colors ? fmt::fg(fmt::color::red) | fmt::emphasis::bold
                            : fmt::text_style{};
  std::fflush(stdout);
  fmt::print(err, style, "{}\n", outcome.error_message);
  // 客户端渲染 caret：服务端只回 span + 原文（它不做 lexer 高亮）
  if (!outcome.error_sql.empty() && sspan_valid(outcome.error_span)) {
    const std::string snippet =
        stmt::highlight_span(outcome.error_sql, outcome.error_span, colors);
    if (!snippet.empty() && snippet != outcome.error_sql) {
      fmt::print(err, "{}\n", snippet);
    } else {
      fmt::print(err, "{}\n", outcome.error_sql);
    }
  }
}

void print_notice(FILE *err, const std::string &text) {
  std::fflush(stdout);
  fmt::print(err, "{}\n", text);
}

void print_grid(FILE *out, const std::vector<std::string> &headers,
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
  fmt::print(out, colors ? fmt::emphasis::bold : fmt::text_style{}, "{}\n",
             header);
  std::string rule;
  for (size_t i = 0; i < headers.size(); ++i) {
    rule += std::string(widths[i], '-');
    rule += (i + 1 == headers.size()) ? "" : "  ";
  }
  fmt::print(out, "{}\n", rule);
  for (const auto &row : rows) {
    std::string text;
    for (size_t i = 0; i < headers.size(); ++i) {
      text += fmt::format("{:<{}}", i < row.size() ? row[i] : std::string(),
                          widths[i]);
      text += (i + 1 == headers.size()) ? "" : "  ";
    }
    fmt::print(out, "{}\n", text);
  }
}

// 结果集（Outcome）打印：行流 -> 表格；写语句 -> OK/受影响行数
int print_outcome(FILE *out, FILE *err, const Outcome &outcome, bool colors) {
  if (!outcome.ok) {
    print_error(err, outcome, colors);
    return 1;
  }
  if (!outcome.has_rows) {
    if (!outcome.is_write) {
      // DDL / USE：只说 OK（和老的 CLI 输出一致）
      fmt::print(out, "OK\n");
      return 0;
    }
    fmt::print(out, "OK, {} row{} affected\n", outcome.affected_rows,
               outcome.affected_rows == 1 ? "" : "s");
    return 0;
  }
  std::vector<std::vector<std::string>> rendered;
  rendered.reserve(outcome.rows.size());
  for (const auto &row : outcome.rows) {
    std::vector<std::string> line;
    line.reserve(outcome.columns.size());
    for (size_t i = 0; i < outcome.columns.size(); ++i) {
      if (i < row.size()) {
        line.push_back(row[i].is_null ? std::string("NULL") : row[i].text);
      } else {
        line.push_back(std::string());
      }
    }
    rendered.push_back(std::move(line));
  }
  print_grid(out, outcome.columns, rendered, colors);
  fmt::print(out, "({} row{})\n", outcome.rows.size(),
             outcome.rows.size() == 1 ? "" : "s");
  return 0;
}

std::string format_time(int64_t unix_seconds) {
  if (unix_seconds <= 0) {
    return "-";
  }
  const std::time_t when = static_cast<std::time_t>(unix_seconds);
  std::tm parts{};
  localtime_r(&when, &parts);
  char buffer[32];
  std::strftime(buffer, sizeof(buffer), "%Y-%m-%d %H:%M:%S", &parts);
  return buffer;
}

// ============================================================
// 元命令
// ============================================================
bool is_meta_command(const std::string &line) {
  size_t begin = 0;
  while (begin < line.size() &&
         std::isspace(static_cast<unsigned char>(line[begin])) != 0) {
    ++begin;
  }
  return begin < line.size() && line[begin] == '\\';
}

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
  const std::string command = line.substr(begin, end - begin);
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

void print_meta_help(FILE *out) {
  fmt::print(out, "元命令：\n");
  fmt::print(out,
             "  \\l                  列出所有数据库（含建库时间、表数量，* "
             "为当前库）\n");
  fmt::print(out,
             "  \\dt                 列出当前库的表（行数/列数/主键/创建时间/"
             "最后写入）\n");
  fmt::print(
      out, "  \\d [表]             不带参数等于 \\dt；带参数看表结构与统计\n");
  fmt::print(out, "  \\c <库>             切换当前数据库（等价 USE <库>）\n");
  fmt::print(out, "  \\begin / \\commit / \\rollback   事务控制（= "
                  "BEGIN/COMMIT/ROLLBACK）\n");
  fmt::print(out, "  \\?                  显示这份帮助\n");
  fmt::print(out, "  exit / quit / \\q    退出\n");
  fmt::print(out, "  EXPLAIN <语句>       只打印计划，不执行\n");
}

// 元命令需要一个"看元信息"的能力：本地连接直接有；远程连接还没有 META 帧
void print_metadata_not_supported(FILE *err) {
  print_notice(err, "远程连接暂不支持该元命令（\\l / \\dt / \\d 走 META 帧，"
                    "下一步补；\\c 可用）");
}

int run_meta_command(SqlConnection &connection, const std::string &line,
                     const ReplOptions &options) {
  const auto [command, argument] = split_meta_line(line);
  if (command == "\\?" || command == "\\h" || command == "\\help") {
    print_meta_help(options.out);
    return 0;
  }
  if (command == "\\c" || command == "\\connect") {
    if (argument.empty()) {
      print_notice(options.err, "用法：\\c <库名>");
      return 1;
    }
    const Outcome outcome = connection.execute("USE " + argument);
    if (!outcome.ok) {
      print_error(options.err, outcome, options.colors);
      return 1;
    }
    fmt::print(options.out, "现在连接的是数据库 \"{}\"\n",
               connection.current_database());
    return 0;
  }
  if (command == "\\begin" || command == "\\commit" ||
      command == "\\rollback") {
    const std::string keyword = command == "\\begin"    ? "BEGIN"
                                : command == "\\commit" ? "COMMIT"
                                                        : "ROLLBACK";
    const Outcome outcome = connection.execute(keyword);
    if (!outcome.ok) {
      print_error(options.err, outcome, options.colors);
      return 1;
    }
    fmt::print(options.out, "{}\n",
               keyword == "BEGIN"    ? "transaction started"
               : keyword == "COMMIT" ? "committed"
                                     : "rolled back");
    return 0;
  }

  // \l / \dt / \d：需要元信息
  const bool wants_metadata = command == "\\l" || command == "\\list" ||
                              command == "\\dt" || command == "\\d";
  if (wants_metadata && !connection.supports_metadata()) {
    print_metadata_not_supported(options.err);
    return 1;
  }
  if (command == "\\l" || command == "\\list") {
    const auto databases = connection.databases();
    if (databases.empty()) {
      fmt::print(options.out, "(没有数据库，用 CREATE DATABASE 建一个)\n");
      return 0;
    }
    std::vector<std::vector<std::string>> rows;
    for (const auto &db : databases) {
      rows.push_back({db.is_current ? "* " + db.name : "  " + db.name,
                      std::to_string(db.table_count),
                      format_time(db.created_at)});
    }
    print_grid(options.out, {"Database", "Tables", "Created"}, rows,
               options.colors);
    return 0;
  }
  if (command == "\\d" && !argument.empty()) {
    std::string db;
    std::string table_name = argument;
    const size_t dot = argument.find('.');
    if (dot != std::string::npos) {
      db = argument.substr(0, dot);
      table_name = argument.substr(dot + 1);
    }
    const std::string target_db =
        db.empty() ? connection.current_database() : db;
    if (target_db.empty()) {
      print_notice(options.err,
                   "当前没有选中数据库（用 USE <库> 或 \\c <库>）");
      return 1;
    }
    auto schema = connection.table_schema(table_name, target_db);
    if (!schema.has_value()) {
      print_notice(options.err, "表不存在：" + target_db + "." + table_name);
      return 1;
    }
    const auto tables = connection.tables(target_db);
    const TableMeta *info = nullptr;
    for (const auto &table : tables) {
      if (table.name == table_name) {
        info = &table;
      }
    }
    fmt::print(options.out, "{}", schema->to_string_table());
    if (info != nullptr) {
      fmt::print(options.out, "统计：rows={}  columns={}  primary key={}\n",
                 info->row_count, info->column_count,
                 info->primary_key.empty() ? "-" : info->primary_key);
      fmt::print(options.out, "      created={}  last write={}\n",
                 format_time(info->created_at),
                 format_time(info->last_write_at));
    }
    return 0;
  }
  if (command == "\\dt" || command == "\\d") {
    const std::string target =
        argument.empty() ? connection.current_database() : argument;
    if (target.empty()) {
      print_notice(options.err,
                   "当前没有选中数据库（用 USE <库> 或 \\c <库>）");
      return 1;
    }
    const auto tables = connection.tables(target);
    if (tables.empty()) {
      fmt::print(options.out, "(库 {} 没有表)\n", target);
      return 0;
    }
    std::vector<std::vector<std::string>> rows;
    for (const auto &table : tables) {
      rows.push_back({table.name, std::to_string(table.row_count),
                      std::to_string(table.column_count),
                      table.primary_key.empty() ? "-" : table.primary_key,
                      format_time(table.created_at),
                      format_time(table.last_write_at)});
    }
    print_grid(
        options.out,
        {"Table", "Rows", "Columns", "Primary key", "Created", "Last write"},
        rows, options.colors);
    return 0;
  }
  print_notice(options.err, "未知元命令：" + command + "（用 \\? 看帮助）");
  return 1;
}

} // namespace

int run_text(SqlConnection &connection, const std::string &text,
             const ReplOptions &options) {
  int failures = 0;
  std::string pending;
  uint32_t pending_start_line = 1;
  uint32_t pending_start_column = 1;

  const auto flush = [&]() {
    if (pending.empty()) {
      return;
    }
    const Statement chunk{pending, pending_start_line, pending_start_column};
    for (const Statement &statement : split_statements(chunk.text)) {
      if (statement.text.find_first_not_of(" \t\r\n") == std::string::npos) {
        continue;
      }
      const Statement absolute{
          statement.text, chunk.start_line + statement.start_line - 1,
          statement.start_line == 1
              ? chunk.start_column + statement.start_column - 1
              : statement.start_column};
      if (options.echo_sql) {
        fmt::print(options.out, "{}\n",
                   stmt::highlight_sql(statement.text, options.colors));
      }
      Outcome outcome = connection.execute(statement.text);
      failures += print_outcome(options.out, options.err,
                                to_absolute(std::move(outcome), absolute),
                                options.colors);
    }
    pending.clear();
  };

  uint32_t line_number = 1;
  size_t begin = 0;
  while (begin <= text.size()) {
    const size_t newline = text.find('\n', begin);
    const std::string line =
        text.substr(begin, newline == std::string::npos ? std::string::npos
                                                        : newline - begin);
    if (pending.empty() && is_meta_command(line)) {
      failures += run_meta_command(connection, line, options);
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
  flush();
  return failures;
}

int run(SqlConnection &connection, const ReplOptions &options,
        const LineReader &reader) {
  const auto prompt = [&]() {
    const std::string db = connection.current_database();
    const std::string name = db.empty() ? "(none)" : db;
    const std::string tag = name + (connection.in_transaction() ? "*" : "");
    return options.colors ? fmt::format("\033[1;36m{}\033[0m> ", tag)
                          : tag + "> ";
  };

  fmt::print(options.out, "sqldb 命令行（{}；输入 exit / quit / \\q 退出）\n",
             connection.description());
  std::string pending;
  while (true) {
    std::string line;
    const std::string prompt_text = pending.empty() ? prompt() : "   ... ";
    if (reader) {
      auto read = reader(prompt_text);
      if (!read.has_value()) {
        break; // Ctrl-D
      }
      line = *read;
    } else {
      fmt::print(options.out, "{}", prompt_text);
      std::fflush(stdout);
      if (!std::getline(std::cin, line)) {
        fmt::print(options.out, "\n");
        break;
      }
    }
    if (pending.empty() && is_exit_command(line)) {
      break;
    }
    if (line.empty() && pending.empty()) {
      continue;
    }
    pending += line + "\n";
    if (line.find(';') == std::string::npos) {
      continue;
    }
    run_text(connection, pending, options);
    pending.clear();
  }
  return 0;
}

} // namespace client
