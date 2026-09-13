
%{
#include "parser.tab.h"
#include "common/c_types.h"
#include "lex_tokens.h"

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* 由 sql.y 实现；词法层发现无法恢复的错误时直接设置错误信息 */
void yyerror(const char* s);

/* flex 生成的扫描函数改名为 lex_yylex；解析器仍然调用 yylex()（下面是包装）。
   这样"收集 token 流"可以直接用原始扫描函数，不干扰解析路径。 */
#define YY_DECL int lex_yylex(void)

/* ============================================================
   位置跟踪（给 Bison 的 @$/@n 提供 first_line/first_column/last_*）

   - 行号自己维护（yy_line），与 flex 的 yylineno 同步；
   - 列号按字节计数，1-based；
   - 只有"可能含换行"的规则（空白、字符串）需要调用 lex_advance()。
   ============================================================ */
static int yy_line = 1;
static int yy_column = 1;

/* 这两个符号由 flex 生成（定义在后面的声明区） */
extern int yylineno;
extern YYLTYPE yylloc;

void lex_reset_location(void) {
    yy_line = 1;
    yy_column = 1;
    yylineno = 1;
}

/* token 开始时先把起止位置填进 yylloc。
   注意：flex 会把本宏当成语句序列直接展开，不能包成 do{...}while(0)。 */
#define YY_USER_ACTION                                  \
    yylloc.first_line = yy_line;                        \
    yylloc.first_column = yy_column;                    \
    yylloc.last_line = yy_line;                         \
    yylloc.last_column = yy_column + (int)yyleng;       \
    yy_column += (int)yyleng;

/* 在动作里调用：修正 token 内部的换行（列号由 YY_USER_ACTION 先推进） */
static void lex_advance(const char* text, int len) {
    int last_newline = -1;
    int i;
    for (i = 0; i < len; ++i) {
        if (text[i] == '\n') {
            yy_line += 1;
            last_newline = i;
        }
    }
    if (last_newline >= 0) {
        yy_column = len - last_newline;   /* 换行后剩余字符数 + 1（1-based 下一列） */
    }
    yylloc.last_line = yy_line;
    yylloc.last_column = yy_column;
}

/* 把 SQL 字符串字面量的内容还原：'' -> ' */
static char* sql_unescape_string(const char* body, size_t len) {
    char* out = (char*)malloc(len + 1);
    size_t i;
    size_t j = 0;
    if (out == NULL) {
        return NULL;
    }
    for (i = 0; i < len; ++i) {
        if (body[i] == '\'' && i + 1 < len && body[i + 1] == '\'') {
            out[j++] = '\'';
            ++i;
        } else {
            out[j++] = body[i];
        }
    }
    out[j] = '\0';
    return out;
}
%}

%option noyywrap
%option case-insensitive
%option yylineno


%%


"select"    { return TOK_SELECT; }
"from"      { return TOK_FROM; }
"where"     { return TOK_WHERE; }

"and"       { return TOK_AND; }
"or"        { return TOK_OR; }
"not"       { return TOK_NOT; }

"insert"    { return TOK_INSERT; }
"into"      { return TOK_INTO; }
"values"    { return TOK_VALUES; }
"update"    { return TOK_UPDATE; }
"set"       { return TOK_SET; }
"delete"    { return TOK_DELETE; }

"order"     { return TOK_ORDER; }
"by"        { return TOK_BY; }
"asc"       { return TOK_ASC; }
"ascending" { return TOK_ASC; }
"desc"      { return TOK_DESC; }
"descending" { return TOK_DESC; }
"limit"     { return TOK_LIMIT; }
"offset"    { return TOK_OFFSET; }

"use"       { return TOK_USE; }
"create"    { return TOK_CREATE; }
"drop"      { return TOK_DROP; }
"database"  { return TOK_DATABASE; }
"table"     { return TOK_TABLE; }
"primary"   { return TOK_PRIMARY; }
"key"       { return TOK_KEY; }

  /* 关键字字面量：必须排在标识符规则之前 */
"null"      { return TOK_NULL; }
"true"      { return TOK_TRUE; }
"false"     { return TOK_FALSE; }

">="        { return TOK_GE; }
"<="        { return TOK_LE; }
">"         { return TOK_GT; }
"<"         { return TOK_LT; }
"="         { return TOK_EQ; }
"!="        { return TOK_NE; }
"<>"        { return TOK_NE; }
"in"        { return TOK_IN; }
"is"        { return TOK_IS; }
"like"      { return TOK_LIKE; }

","         { return ','; }
"("         { return '('; }
")"         { return ')'; }
";"         { return ';'; }
"*"         { return '*'; }
"%"         { return '%'; }
"-"         { return '-'; }

  /* 类型名由 common/c_types.h 的别名表判定（唯一数据源），
   不再逐个类型写关键字规则：新增类型只改那张表。 */
[a-zA-Z_][a-zA-Z0-9_]* {
    if (c_type_lookup(yytext) != DT_UNKNOWN) {
        yylval.str = strdup(yytext);
        return TOK_TYPE_NAME;
    }
    yylval.str = strdup(yytext);
    return TOK_IDENT;
}

  /* 整数：按 int64 解析，超出范围直接报错（不再被 atoi 截断） */
[0-9]+ {
    char* endptr = NULL;
    long long value;
    errno = 0;
    value = strtoll(yytext, &endptr, 10);
    if (errno == ERANGE || endptr == yytext || *endptr != '\0') {
        yyerror("integer literal out of range");
        return TOK_LEX_ERROR;
    }
    yylval.i64 = (int64_t)value;
    return TOK_NUMBER;
}

  /* 字符串：支持标准 SQL 的 '' 转义 */
'([^']|[']{2})*' {
    yylval.str = sql_unescape_string(yytext + 1, (size_t)(strlen(yytext) - 2));
    lex_advance(yytext, (int)yyleng);   /* 字符串可能跨行 */
    return TOK_STRING;
}

[ \t\n\r]+ { lex_advance(yytext, (int)yyleng); }   /* 空白：只推进位置 */

  /* 注释：SQL 标准两种写法。注释不产生 token（与 lex_tokens.h 的约定一致），
   高亮器按"相邻 token 之间的空隙"原样复制，所以注释仍然会显示出来。 */
"--"[^\n]* { lex_advance(yytext, (int)yyleng); }            /* 行注释 */
"/*"([^*]|\*+[^*/])*\*+"/" { lex_advance(yytext, (int)yyleng); } /* 块注释（可跨行） */

  /* 其它字符：交给语法层报错，不能静默丢弃 */
. {
    return yytext[0];
}

%%

/* ============================================================
   token 流导出（供高亮/工具使用；与解析共用同一套词法规则）
   ============================================================ */

/* 解析器调用的入口：转发到 flex 生成的 lex_yylex */
int yylex(void) { return lex_yylex(); }

int lex_collect_tokens(const char* sql, LexToken** out) {
    size_t sql_len;
    char* base;
    YY_BUFFER_STATE buffer;
    LexToken* tokens = NULL;
    int count = 0;
    int capacity = 0;

    if (sql == NULL || out == NULL) {
        return -1;
    }
    *out = NULL;

    sql_len = strlen(sql);
    /* yy_scan_buffer 要求缓冲区可变，并以两个 EOB 字符结尾
       （同时让我们能用指针差算出 offset）。 */
    base = (char*)malloc(sql_len + 2);
    if (base == NULL) {
        return -1;
    }
    memcpy(base, sql, sql_len);
    base[sql_len] = '\0';
    base[sql_len + 1] = '\0';

    lex_reset_location();
    buffer = yy_scan_buffer(base, sql_len + 2);
    if (buffer == NULL) {
        free(base);
        return -1;
    }

    for (;;) {
        const int kind = lex_yylex();
        LexToken* grown;
        int next_capacity;

        if (kind == 0) {
            break; /* EOF */
        }
        if (count == capacity) {
            next_capacity = (capacity == 0) ? 64 : capacity * 2;
            grown = (LexToken*)realloc(tokens,
                                       (size_t)next_capacity * sizeof(LexToken));
            if (grown == NULL) {
                free(tokens);
                yy_delete_buffer(buffer);
                free(base);
                return -1;
            }
            tokens = grown;
            capacity = next_capacity;
        }
        tokens[count].kind = kind;
        tokens[count].span = sspan_make(
            (uint32_t)yylloc.first_line, (uint32_t)yylloc.first_column,
            (uint32_t)yylloc.last_line, (uint32_t)yylloc.last_column);
        tokens[count].offset = (size_t)(yytext - base);
        tokens[count].length = (size_t)yyleng;
        count++;
    }

    yy_delete_buffer(buffer);
    free(base);
    *out = tokens;
    return count;
}
