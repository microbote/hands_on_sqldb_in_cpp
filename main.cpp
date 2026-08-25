#include <iostream>
#include <string>
#include <sstream>
#include <optional>
#include <vector>
#include <algorithm>
#include <readline/readline.h>
#include <readline/history.h>
#include <leveldb/db.h>
#include <fmt/core.h>
#include <fmt/color.h>

#include "ast.h"
#include "statement.h"
#include "plan.h"
#include "catalog.h"
#include "executor.h"

#include "parser.h"


// 命令历史记录
class History {
public:
    void add(const std::string& cmd) {
        if (cmd.empty()) return;
        add_history(cmd.c_str());
    }

    void show(std::vector<std::string> entries) const {
        if (entries.empty()) {
            fmt::print("(history is empty)\n");
            return;
        }
        for (size_t i = 0; i < entries.size(); ++i) {
            fmt::print("{:>4}  {}\n", i + 1, entries[i]);
        }
    }
};

// 解析一行输入，返回命令和参数
static std::pair<std::string, std::string> parse_line(const std::string& line) {
  size_t start = line.find_first_not_of(" \t");
  if (start == std::string::npos) return {"", ""};

  size_t end = line.find_first_of(" \t", start);
  if (end == std::string::npos) {
    return {line.substr(start), ""};
  }

  std::string cmd = line.substr(start, end - start);

  size_t args_start = line.find_first_not_of(" \t", end);
  std::string args;
  if (args_start != std::string::npos) {
    args = line.substr(args_start);
  }

  return {cmd, args};
}

static std::vector<std::string> get_history() {
    std::vector<std::string> entries;
    int n = history_length;
    HIST_ENTRY** h = history_list();
    for (int i = 0; i < n; ++i) {
        if (h[i]) entries.push_back(h[i]->line);
    }
    return entries;
}


int main() {
    std::unique_ptr<leveldb::DB> db = nullptr;
    std::string db_path;
    Catalog catalog;
    History history;

    fmt::print(fg(fmt::color::cyan), "========================================\n");
    fmt::print(fg(fmt::color::cyan), "  SQL Engine Shell (C++20 + fmt)\n");
    fmt::print(fg(fmt::color::cyan), "  Commands: open, close, select, insert, update, delete, history, quit\n");
    fmt::print(fg(fmt::color::cyan), "========================================\n");

    while (true) {
        // 提示符：如果 db 已打开，显示路径
        std::string prompt = "sql> ";
        if (db != nullptr) {
            prompt = fmt::format(fg(fmt::color::green), "({}) sql> ", db_path);
        }
        // readline() 返回的是 char*（裸指针）。std::move
        // 对裸指针没有任何实际意义，而 std::string 的赋值运算符接收的是 const
        // char*。这会导致指针被隐式构造为临时 std::string
        // 对象，随后临时对象被销毁，指针指向的内存被释放。后续对 line
        // 的操作实际上是在读取已释放的内存，因此出现了 qui t、his tory
        // 这种乱码截断现象。
        char * rawline = readline(prompt.c_str());
        if (rawline == nullptr) break;
        std::string trimmed = rawline;
        free(rawline);

        auto [cmd, args] = parse_line(trimmed);
        if (cmd.empty()) {
            fmt::print("No command\n");
            continue;
        }
        trimmed = cmd + " " + args;
        add_history(trimmed.c_str());

        // quit / exit
        if (cmd == "quit" || cmd == "exit") {
            if (db != nullptr) {
                db = nullptr;
                fmt::print("Database closed automatically.\n");
            }
            fmt::print("Bye!\n");
            break;
        } else if (cmd == "open") {
            std::string path;
            std::istringstream iss(args);
            iss >> path;
            if (path.empty()) {
                fmt::print("Usage: open <db_path>\\n");
                continue;
            }
            if (db != nullptr) {
                db.release();
            }
            leveldb::Options options;
            options.create_if_missing = true;
            leveldb::DB * tdb = nullptr;
            leveldb::Status s = leveldb::DB::Open(options, path, &tdb);
            if (!s.ok()) {
                fmt::print(stderr, "Open failed: {}\n", s.ToString());
            } else {
                db.reset(tdb);
                db_path = path;
                fmt::print("OK: opened {}\n", path);
            }
            continue;
        } else if (cmd == "close") {
            if (db == nullptr) {
                fmt::print("No database is open.\n");
                continue;
            }
            fmt::print("Database [{}]closed.\n", db_path);
            db.release();
            db_path.clear();
            continue;
        }else if (cmd == "history") {
            // 使用 readline 的历史记录 + 内置 history 命令
            auto entries = get_history();
            history.show(entries);
            continue;
        } else { // sql parser
            // 1. 检查数据库是否打开
            if (!db) {
                fmt::print(fg(fmt::color::red),
                       "Error: No database opened. Use 'open <path>' first.\n");
                continue;
            }
            // 2. 将 sql 语句解析成 AST
            std::string sql = trimmed;
            if(sql.empty()){
                fmt::print(fg(fmt::color::red), "Syntax error: {}\n", sql);
                continue;
            }
            if (sql.back() != ';') {
                sql.push_back(';');
            }

            YY_BUFFER_STATE buffer = yy_scan_string(sql.c_str());

            int ret = yyparse();
            // 清理 Flex 缓冲区 (防止内存泄漏)
            yy_delete_buffer(buffer);

            if (ret != 0) {
              fmt::print("Syntax error: {}\n", sql);
              reset_parse_state();
              continue;
            }

            // 3. 将 AST 解析成 Plan
            ASTNode* ast = get_parsed_ast();
            if (ast == nullptr) {
                fmt::print("Parse failed: {}\n", sql);
                reset_parse_state();
                continue;
            }

            try {
                auto stmt = build_statement(ast);
                auto plan = make_plan(db, stmt);
                execute(plan);
            } catch (const std::exception& e) {
                fmt::print("Runtime Error: {}\n", e.what());
            }

            reset_parse_state();
        }
    }
    fmt::print("Bye! Quiting...\n");
    return 0;
}


