#ifndef PARSER_LEX_TOKENS_H
#define PARSER_LEX_TOKENS_H

// 词法 token 流导出（供语法高亮/工具使用）
//
// 设计：**词法规则只有一处** —— sql.l。高亮器等展示层不再自己扫描 SQL，
// 而是消费这里导出的 token 流，按 kind 上色：
//   - 新增关键字：只改 sql.l / sql.y，高亮自动跟随（不会出现"解析器认、
//     高亮器不认"的漂移）；
//   - 字符串转义、数字格式、注释等规则同理只有一份。
//
// 注意：
//   - 空白与注释不会产生 token；调用方用相邻 token 的 offset 之间的空隙
//     还原原文（这样高亮输出与输入逐字节一致）；
//   - 收集过程会自行建立/销毁扫描缓冲，**不要在 yyparse() 进行中调用**；
//   - 返回的数组由 malloc 分配，调用方负责 free()。

#include <stddef.h>

#include "common/source_span.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct LexToken {
  int kind;      // TOK_* 常量（见 parser.tab.h）；单字符 token 就是字符码
  SSpan span;    // 位置（1-based，列区间左闭右开）
  size_t offset; // 在传入 SQL 文本中的字节偏移
  size_t length; // 字节长度
} LexToken;

// 收集 sql 的全部词法 token；返回 token 个数，失败返回 -1。
int lex_collect_tokens(const char *sql, LexToken **out);

#ifdef __cplusplus
}
#endif

#endif /* PARSER_LEX_TOKENS_H */
