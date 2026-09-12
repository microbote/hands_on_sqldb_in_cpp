// statement/source_span.h
//
// SourceSpan 相关的 C++ 工具：
//   - 把 SSpan 打印成 "line 1:8-10"
//   - 按 span 在 SQL 文本下画 caret（CLI 报错用）
//   - 从 AST 里收集"名字 -> 位置"，供 StatementValidator 给错误附位置
//
// 位置信息全部来自 parser：sql.l 维护行列，sql.y 用 Bison 的 @$/@n
// 把位置写进 AST 节点（见 ASTNode::span）。
#pragma once

#include <cstddef>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

#include "common/ansi_color.h"
#include "common/source_span.h"
#include "parser/ast.h"
#include "sql_types/identifier.h"

namespace stmt {

// "line 1:8-10"；未知位置返回空串
inline std::string sspan_to_string(SSpan span) {
  if (!sspan_valid(span)) {
    return std::string();
  }
  std::string text = "line " + std::to_string(span.begin_line) + ":" +
                     std::to_string(span.begin_column);
  if (span.end_line == span.begin_line && span.end_column > span.begin_column) {
    text += "-" + std::to_string(span.end_column);
  } else if (span.end_line > span.begin_line) {
    text += ".." + std::to_string(span.end_line) + ":" +
            std::to_string(span.end_column);
  }
  return text;
}

// 取出 SQL 文本里某一行（1-based，不含换行符）
inline std::string_view sspan_line_text(std::string_view sql, uint32_t line) {
  if (line == 0) {
    return {};
  }
  size_t begin = 0;
  uint32_t current = 1;
  while (current < line) {
    const size_t next = sql.find('\n', begin);
    if (next == std::string_view::npos) {
      return {};
    }
    begin = next + 1;
    ++current;
  }
  const size_t end = sql.find('\n', begin);
  return sql.substr(begin, end == std::string_view::npos
                               ? std::string_view::npos
                               : end - begin);
}

// 在 SQL 文本下画 caret，例如：
//   SELECT * FROM users WHERE age > 18
//                           ^^^
//   line 1:28-31
// colors = true 时：出错片段标亮红、caret 与位置信息着色；
// 默认 false（由调用方决定，CLI 可以传 sql::ansi::enabled_by_default()）。
inline std::string sspan_caret(std::string_view sql, SSpan span,
                               const std::string &label = {},
                               bool colors = false) {
  if (!sspan_valid(span)) {
    return std::string();
  }
  const std::string_view line_text = sspan_line_text(sql, span.begin_line);
  if (line_text.empty() && span.begin_line != 1) {
    return std::string();
  }

  // 跨行时只标注起始行的剩余部分
  const size_t begin = span.begin_column > 0 ? span.begin_column - 1 : 0;
  size_t width = sspan_width(span);
  if (width == 0) {
    width = 1;
  }
  std::string out;
  out += sql::ansi::paint("  ", sql::ansi::kDim, colors);
  // 行内高亮 span 覆盖的片段（注意：这里是"行内坐标"，用 begin_column-1 起的
  // width）
  {
    const size_t line_len = line_text.size();
    const size_t hl_begin = begin <= line_len ? begin : line_len;
    const size_t hl_end =
        (hl_begin + width) <= line_len ? (hl_begin + width) : line_len;
    out.append(line_text.substr(0, hl_begin));
    out += sql::ansi::paint(line_text.substr(hl_begin, hl_end - hl_begin),
                            sql::ansi::kErrorSpan, colors);
    out.append(line_text.substr(hl_end));
  }
  out += "\n  ";
  out.append(begin, ' ');
  out +=
      sql::ansi::paint(std::string(width, '^'), sql::ansi::kErrorSpan, colors);
  const std::string where = sspan_to_string(span);
  if (!label.empty()) {
    out += " " + sql::ansi::paint(label, sql::ansi::kRed, colors);
  }
  if (!where.empty()) {
    out += sql::ansi::paint(" (" + where + ")", sql::ansi::kDim, colors);
  }
  return out;
}

// 名字 -> 位置（名字大小写不敏感，与 Identifier 一致）
using SpanResolver = std::function<SSpan(const sql::Identifier &)>;

struct NamedSpan {
  sql::Identifier name;
  SSpan span;
};

// 收集 AST 里出现的所有"名字 -> 位置"：表名/库名/列名
std::vector<NamedSpan> collect_source_spans(const ASTNode *ast);

// 便捷：把收集结果包成一个 resolver（未命中返回未知 span）
SpanResolver make_span_resolver(const ASTNode *ast);

} // namespace stmt
