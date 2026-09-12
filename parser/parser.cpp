// parser.cpp
#include "parser.h"
#include <cstring>
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
extern int ast_debug;

namespace parser {

// ============================================================
// 解析器全局状态（用于错误回调）
// ============================================================
struct ParserGlobals {
  Parser *instance = nullptr;
  std::string error_buffer;
  bool has_error = false;
};

static ParserGlobals g_parser_state;

// ============================================================
// 错误回调函数（由 Bison 调用）
// ============================================================
#ifdef  __cplusplus
extern "C" {
#endif 
void yyerror(const char *s) {
  if (g_parser_state.instance != nullptr) {
    // 只记录第一个错误：词法层报出的具体错误（例如整数字面量越界）
    // 不应该被随后的 "syntax error" 覆盖。
    if (!g_parser_state.has_error) {
      std::string error = s;
      if (yylineno > 0) {
        error = "line " + std::to_string(yylineno) + ": " + error;
      }
      g_parser_state.instance->set_last_error(error);
      g_parser_state.error_buffer = error;
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
Parser::Parser() {
  // 注册当前实例到全局状态
  g_parser_state.instance = this;
  g_parser_state.error_buffer.clear();
  g_parser_state.has_error = false;

  log("[Parser] Created");
}

Parser::~Parser() {
  log("[Parser] Destroyed (parses: " + std::to_string(parse_count_) +
      ", errors: " + std::to_string(error_count_) + ")");

  // 清理全局状态
  g_parser_state.instance = nullptr;
}

ParseResult Parser::parse(const std::string &sql) {
  // 重置状态
  last_error_.clear();
  g_parser_state.error_buffer.clear();
  g_parser_state.has_error = false;

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
  return ParseResult(sql, ParseError(last_error_));
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

void Parser::reset() {
  last_error_.clear();
  g_parser_state.error_buffer.clear();
  g_parser_state.has_error = false;
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
