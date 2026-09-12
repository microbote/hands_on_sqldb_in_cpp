// statement/sql_highlight.cpp
//
// SQL 高亮的实现原则：**不自己扫描 SQL**。
// 词法规则只有一份（parser/sql.l），这里消费 lex_collect_tokens() 导出的
// token 流，按 token kind 上色；token 之间的空隙（空白、注释）原样复制，
// 因此输出与输入逐字节一致。
#include "sql_highlight.h"

#include <cstdlib>
#include <string>
#include <string_view>

#include "common/ansi_color.h"
#include "parser/lex_tokens.h"
#include <parser.tab.h> // TOK_* 常量

namespace stmt {

namespace {

// Bison 为用户 token 分配的第一个编号（0..257 留给字符码）
constexpr int kFirstTokenKind = 258;

// token 种类 -> 颜色。返回空表示"不上色"。
//
// 注意这里**没有关键字清单**：凡是 lexer 认出的 TOK_*（>= 258）且不属于
// 下面这些特例的，都按关键字上色。因此 sql.l 里新增一个关键字，
// 高亮自动跟随，不需要改这里。
std::string_view color_for(int kind) {
  switch (kind) {
  case TOK_NUMBER:
    return sql::ansi::kYellow;
  case TOK_STRING:
    return sql::ansi::kGreen;
  case TOK_TYPE_NAME:
    return sql::ansi::kCyan;
  case TOK_NULL:
  case TOK_TRUE:
  case TOK_FALSE:
    return sql::ansi::kMagenta; // 字面量
  case TOK_LEX_ERROR:
    return sql::ansi::kErrorSpan; // 词法错误：直接标红
  case TOK_IDENT:
    return {}; // 标识符不上色
  case TOK_EQ:
  case TOK_NE:
  case TOK_GT:
  case TOK_GE:
  case TOK_LT:
  case TOK_LE:
    return sql::ansi::kBold; // 比较运算符（中性色，避免与报错红混淆）
  default:
    break;
  }
  if (kind >= kFirstTokenKind) {
    return sql::ansi::kKeyword; // 其它 TOK_*：关键字（含 IN/IS/LIKE）
  }
  return {}; // 单字符 token：标点/空白等，原样
}

} // namespace

std::string highlight_sql(std::string_view sql, bool colors) {
  if (sql.empty()) {
    return std::string();
  }

  // lex_collect_tokens 需要以 NUL 结尾的字符串
  const std::string text(sql);
  LexToken *tokens = nullptr;
  const int count = lex_collect_tokens(text.c_str(), &tokens);
  if (count <= 0 || tokens == nullptr) {
    std::free(tokens);
    return text; // 词法阶段没有 token：原样返回
  }

  std::string out;
  out.reserve(sql.size() + 64);
  size_t cursor = 0;
  for (int i = 0; i < count; ++i) {
    const LexToken &token = tokens[i];
    if (token.offset > cursor) {
      // token 之间的空隙（空白/注释）：原样复制，保证输出与输入一致
      out.append(sql.substr(cursor, token.offset - cursor));
    }
    const std::string_view piece = sql.substr(token.offset, token.length);
    out += sql::ansi::paint(piece, color_for(token.kind), colors);
    cursor = token.offset + token.length;
  }
  if (cursor < sql.size()) {
    out.append(sql.substr(cursor));
  }

  std::free(tokens);
  return out;
}

std::string highlight_span(std::string_view sql, SSpan span, bool colors) {
  if (!sspan_valid(span)) {
    return std::string(sql);
  }

  // 找到 span 起始行的范围
  size_t line_begin = 0;
  uint32_t line = 1;
  while (line < span.begin_line) {
    const size_t next = sql.find('\n', line_begin);
    if (next == std::string_view::npos) {
      return std::string(sql);
    }
    line_begin = next + 1;
    ++line;
  }
  size_t line_end = sql.find('\n', line_begin);
  if (line_end == std::string_view::npos) {
    line_end = sql.size();
  }

  const size_t begin =
      line_begin + (span.begin_column > 0 ? span.begin_column - 1 : 0);
  size_t end = begin;
  if (span.end_line == span.begin_line && span.end_column > span.begin_column) {
    end = line_begin + span.end_column - 1;
  }
  if (begin > sql.size() || end > sql.size() || end < begin) {
    return std::string(sql);
  }

  std::string out;
  out.reserve(sql.size() + 32);
  out.append(sql.substr(0, begin));
  out.append(sql::ansi::paint(sql.substr(begin, end - begin),
                              sql::ansi::kErrorSpan, colors));
  out.append(sql.substr(end));
  return out;
}

} // namespace stmt
