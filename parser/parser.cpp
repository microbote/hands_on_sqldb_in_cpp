// parser.cpp
#include "parser.h"
#include <atomic>
#include <cstring>
#include <mutex>
#include <sstream>
#include <vector>

// 生成代码按 C 编译，因此把 flex/bison 的头文件整体放进 extern "C"
// 用尖括号：只在 -I 路径（build/parser 或 parser/）里查找，
// 不会被源码目录里可能残留的旧生成文件优先命中。
extern "C" {
#include <lex.yy.h>
#include <parser.tab.h>
// lex.yy.h 没有导出这个调试开关，但 lex.yy.c 里定义了它
extern int yy_flex_debug;
// 在 sql.l 里实现：重置行列计数（每次解析前调用）
void lex_reset_location(void);
}

// ast.cpp 里的调试开关（C++ 链接）
extern std::atomic<int> ast_debug;

namespace parser {

// ============================================================
// 解析串行化（P0：服务端多线程安全）
//
// 词法/语法层用的是**进程级全局状态**：
//   - flex 的扫描缓冲/yytext/yyleng/yylloc/yylineno（sql.l 非重入）；
//   - ast.cpp 的 `g_parsed_ast`（语法动作 set_parsed_ast 写它）；
//   - 本文件的 `g_parser_state`（yyerror 的错误接收方）。
// 所以同一时刻只能有一次解析在跑：整个 parse 操作（登记错误接收方 ->
// yyparse -> 取走 AST -> 摘掉接收方）都在这把锁里。
//
// 代价与后续：解析一条语句是微秒级，而我们的引擎是"单写者 + 同步执行"，
// 这把锁远不是瓶颈；等 M3 需要真并行解析时，再按 `parser/README.md`
// 的"可重入化清单"把 bison 改成纯解析器（%define api.pure）+ flex
// %option reentrant，然后把这把锁删掉（那时 `lex_collect_tokens` 也一并
// 变成每次调用自己的扫描器）。
// ============================================================
std::mutex &parse_mutex() {
  static std::mutex mutex;
  return mutex;
}

// ============================================================
// 解析器全局状态（用于错误回调）
// ============================================================
struct ParserGlobals {
  Parser *instance = nullptr;
  std::string error_buffer;
  bool has_error = false;
  // 第一个错误的位置（1-based，0 = 未知）。lexer 已经用 YY_USER_ACTION
  // 维护了 yylloc，这里只是把它一起带出来，供上层（session/CLI）
  // 定位并高亮出错片段。
  int error_line = 0;
  int error_column = 0;
  int error_end_line = 0;
  int error_end_column = 0;
};

static ParserGlobals g_parser_state;

// ============================================================
// 错误回调函数（由 Bison 调用）
// ============================================================
#ifdef __cplusplus
extern "C" {
#endif
void yyerror(const char *s) {
  if (g_parser_state.instance != nullptr) {
    // 只记录第一个错误：词法层报出的具体错误（例如整数字面量越界）
    // 不应该被随后的 "syntax error" 覆盖。
    if (!g_parser_state.has_error) {
      // 位置信息单独记录，不再拼进 message 文本：
      // 这样调用方既能拿到纯信息，也能拿到 (line, column) 去高亮。
      g_parser_state.instance->set_last_error(s);
      g_parser_state.error_buffer = s;
      g_parser_state.error_line = static_cast<int>(yylloc.first_line);
      g_parser_state.error_column = static_cast<int>(yylloc.first_column);
      g_parser_state.error_end_line = static_cast<int>(yylloc.last_line);
      g_parser_state.error_end_column = static_cast<int>(yylloc.last_column);
      g_parser_state.has_error = true;
    }

    // 调试模式下走日志回调（默认无输出）
    if (g_parser_state.instance->debug_enabled()) {
      g_parser_state.instance->log_message(std::string("[Parser] ") + s);
    }
  }
}
#ifdef __cplusplus
}
#endif

// ============================================================
// Parser 实现
// ============================================================
Parser::Parser() { log("[Parser] Created"); }

Parser::~Parser() {
  log("[Parser] Destroyed (parses: " + std::to_string(parse_count_) +
      ", errors: " + std::to_string(error_count_) + ")");
}

ParseResult Parser::parse(const std::string &sql) {
  // 全局词法/语法状态：整个解析串行化（见文件头的说明）
  std::lock_guard<std::mutex> lock(parse_mutex());

  // 只在本条语句的解析期间登记错误接收方：解析结束立刻摘掉，
  // 免得把另一个 Parser 的 (this) 留在全局状态里。
  g_parser_state.instance = this;
  struct Unregister {
    ~Unregister() { g_parser_state.instance = nullptr; }
  } unregister;

  // 重置状态（已持锁，用不加锁的版本）
  clear_error_state();

  ASTNode *raw_ast = nullptr;
  bool success = do_parse(sql, &raw_ast);
  parse_count_++;

  if (success && (raw_ast != nullptr)) {
    log("[Parser] Parse successful");
    return ParseResult(sql, raw_ast);
  }
  error_count_++;
  if (last_error_.empty() && !g_parser_state.error_buffer.empty()) {
    last_error_ = g_parser_state.error_buffer;
  }
  if (last_error_.empty()) {
    last_error_ = "Syntax error";
  }
  log("[Parser] Parse failed: " + last_error_);
  return ParseResult(sql, ParseError(g_parser_state.error_line,
                                     g_parser_state.error_column, last_error_));
}

std::vector<ParseResult> Parser::parse_multi(const std::string &sql) {
  std::vector<ParseResult> results;

  // 按分号分割
  std::stringstream ss(sql);
  std::string statement;
  size_t stmt_count = 0;

  while (std::getline(ss, statement, ';')) {
    // 去除首尾空白
    size_t start = statement.find_first_not_of(" \t\n\r");
    if (start == std::string::npos) {
      continue;
    }
    statement = statement.substr(start);

    size_t end = statement.find_last_not_of(" \t\n\r");
    if (end != std::string::npos) {
      statement = statement.substr(0, end + 1);
    }

    if (!statement.empty()) {
      stmt_count++;
      std::string sql_stmt = statement + ";";
      log("[Parser] Parsing statement [" + std::to_string(stmt_count) +
          "]: " + sql_stmt);
      results.push_back(parse(sql_stmt));
    }
  }

  log("[Parser] Parsed " + std::to_string(stmt_count) + " statements");
  return results;
}

bool Parser::do_parse(const std::string &sql, ASTNode **result) {
  if (result == nullptr) {
    last_error_ = "Invalid result pointer";
    return false;
  }

  *result = nullptr;

  if (debug_) {
    log("[Parser] Parsing: " + sql);
    // 启用 Flex 调试输出
    yy_flex_debug = 1;
    ast_debug = 1;
  } else {
    yy_flex_debug = 0;
    ast_debug = 0;
  }

  // 重置位置跟踪（行号 + 列号），供 Bison 的 @$/@n 使用
  lex_reset_location();

  // 扫描 SQL 字符串
  YY_BUFFER_STATE buffer = yy_scan_string(sql.c_str());
  if (buffer == nullptr) {
    last_error_ = "Failed to create scanner buffer";
    return false;
  }

  // 执行解析
  int parse_result = yyparse();

  // 清理 Flex 缓冲区
  yy_delete_buffer(buffer);

  // 检查解析结果
  if (parse_result != 0 || (g_parsed_ast == nullptr)) {
    if (last_error_.empty() && !g_parser_state.error_buffer.empty()) {
      last_error_ = g_parser_state.error_buffer;
    }
    if (last_error_.empty()) {
      last_error_ =
          "Syntax error (parse_result=" + std::to_string(parse_result) + ")";
    }
    if (debug_) {
      log("[Parser] Parse failed: " + last_error_);
    }
    return false;
  }

  // 转移 AST 所有权
  *result = g_parsed_ast;
  g_parsed_ast = nullptr;

  if (debug_) {
    log("[Parser] Parse succeeded, AST at " +
        std::to_string(reinterpret_cast<uintptr_t>(*result)));
  }

  return true;
}

// ============================================================
// 辅助函数
// ============================================================
std::string Parser::error_detail() const {
  if (last_error_.empty()) {
    return "No error";
  }
  return last_error_;
}

void Parser::clear_error_state() {
  last_error_.clear();
  g_parser_state.error_buffer.clear();
  g_parser_state.has_error = false;
  g_parser_state.error_line = 0;
  g_parser_state.error_column = 0;
  g_parser_state.error_end_line = 0;
  g_parser_state.error_end_column = 0;
}

void Parser::reset() {
  // 全局状态：和 parse() 共用同一把锁（本函数可能在别的线程调用）
  std::lock_guard<std::mutex> lock(parse_mutex());
  clear_error_state();
}

// ============================================================
// 全局便捷函数
// ============================================================
ASTNode *parse_sql(const std::string &sql, std::string *error) {
  Parser parser;
  auto result = parser.parse(sql);

  if (!result.success) {
    if (error != nullptr) {
      *error = result.error ? result.error->to_string() : "Unknown error";
    }
    return nullptr;
  }

  return result.release();
}

ASTNodePtr parse_sql_smart(const std::string &sql, std::string *error) {
  Parser parser;
  auto result = parser.parse(sql);

  if (!result.success) {
    if (error != nullptr) {
      *error = result.error ? result.error->to_string() : "Unknown error";
    }
    return nullptr;
  }

  // 转移所有权到 unique_ptr
  return ASTNodePtr(result.release());
}

} // namespace parser
