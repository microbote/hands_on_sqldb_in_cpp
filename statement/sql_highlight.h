// statement/sql_highlight.h
//
// SQL 文本的语法高亮（ANSI）。用途：
//   - 报错时把出问题的片段标红（配合 sspan_caret）
//   - CLI 回显 SQL 时着色
//
// 实现原则：**不自己扫描 SQL**。词法规则只有一份（parser/sql.l），
// highlight_sql() 消费 lex_collect_tokens() 导出的 token 流，按 token kind
// 上色；token 之间的空隙（空白/注释）原样复制，输出与输入逐字节一致。
// 因此 sql.l 新增关键字时高亮自动跟随，不存在"两套关键字表"的漂移。
//
// 高亮是展示层的东西，不参与语义；colors=false 时原样返回。
#pragma once

#include <string>
#include <string_view>

#include "common/source_span.h"

namespace stmt {

// 整条 SQL 着色：关键字/类型/字符串/数字/注释/标点
std::string highlight_sql(std::string_view sql, bool colors);

// 把 sql 里 span 覆盖的那一段标成红色（其余部分保持原样），
// 用于错误提示行。跨行时只处理起始行。
std::string highlight_span(std::string_view sql, SSpan span, bool colors);

} // namespace stmt
