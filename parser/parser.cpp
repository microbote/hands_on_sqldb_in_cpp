// parser.cpp
#include "parser.h"
#include "lex.yy.h"
#include "parser.tab.h"
#include <cstring>
#include <iostream>
#include <sstream>
#include <vector>

// Flex/Bison 外部声明
extern int yyparse();
extern void yy_delete_buffer(YY_BUFFER_STATE buffer);
extern YY_BUFFER_STATE yy_scan_string(const char *str);
extern ASTNode *g_parsed_ast;
extern int yylineno;
extern int yy_flex_debug;
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
    std::string error = s;
    if (yylineno > 0) {
      error = "line " + std::to_string(yylineno) + ": " + error;
    }
    g_parser_state.instance->set_last_error(error);
    g_parser_state.error_buffer = error;
    g_parser_state.has_error = true;
  }

  // 调试模式下输出到 stderr
  if ((g_parser_state.instance != nullptr) &&
      g_parser_state.instance->debug_enabled()) {
    std::cerr << "[Parser] " << s << '\n';
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

  std::cout << "[Parser] Created" << '\n';
}

Parser::~Parser() {
  std::cout << "[Parser] Destroyed (parses: " << parse_count_
            << ", errors: " << error_count_ << ")" << '\n';

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
    std::cout << "[Parser] Parse successful" << '\n';
    return ParseResult(sql, raw_ast);
  }
  error_count_++;
  if (last_error_.empty() && !g_parser_state.error_buffer.empty()) {
    last_error_ = g_parser_state.error_buffer;
  }
  if (last_error_.empty()) {
    last_error_ = "Syntax error";
  }
  std::cout << "[Parser] Parse failed: " << last_error_ << '\n';
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
      std::cout << "[Parser] Parsing statement [" << stmt_count << "]: "
                << sql_stmt << '\n';
      results.push_back(parse(sql_stmt));
    }
  }

  std::cout << "[Parser] Parsed " << stmt_count << " statements" << '\n';
  return results;
}

bool Parser::do_parse(const std::string &sql, ASTNode **result) {
  if (result == nullptr) {
    last_error_ = "Invalid result pointer";
    return false;
  }

  *result = nullptr;

  if (debug_) {
    std::cout << "[Parser] Parsing: " << sql << '\n';
    // 启用 Flex 调试输出
    yy_flex_debug = 1;
    ast_debug = 1;
  } else {
    yy_flex_debug = 0;
    ast_debug = 0;
  }

  // 重置行号
  yylineno = 1;

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
      std::cout << "[Parser] Parse failed: " << last_error_ << '\n';
    }
    return false;
  }

  // 转移 AST 所有权
  *result = g_parsed_ast;
  g_parsed_ast = nullptr;

  if (debug_) {
    std::cout << "[Parser] Parse succeeded, AST at " << (void *)*result
              << '\n';
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