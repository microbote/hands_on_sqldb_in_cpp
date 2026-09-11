
%{
#include "parser.tab.h"
#include "common/c_types.h"

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* 由 sql.y 实现；词法层发现无法恢复的错误时直接设置错误信息 */
void yyerror(const char* s);

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
    return TOK_STRING;
}

[ \t\n\r]+ { /* 忽略空白（yylineno 由 flex 维护） */ }

  /* 其它字符：交给语法层报错，不能静默丢弃 */
. {
    return yytext[0];
}

%%
