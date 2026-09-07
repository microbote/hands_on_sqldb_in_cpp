
%{
#include "parser.tab.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

void debug_token(const char* name, int token) {
    printf("DEBUG: %s -> token %d\n", name, token);
}
%}

%option noyywrap
%option case-insensitive


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

"int"       { return TOK_INT; }
"integer"   { return TOK_INT; }
"bigint"    { return TOK_BIGINT; }
"varchar"   { return TOK_VARCHAR; }
"text"      { return TOK_TEXT; }
"boolean"   { return TOK_BOOLEAN; }
"bool"      { return TOK_BOOLEAN; }
"null"      { return TOK_NULL; }

">="        { return TOK_GE; }
"<="        { return TOK_LE; }
">"         { return TOK_GT; }
"<"         { return TOK_LT; }
"="         { return TOK_EQ; }
"!="        { return TOK_NE; }
"in"        { return TOK_IN; }
"is"        { return TOK_IS; }
"like"      { return TOK_LIKE; }

","         { return ','; }
"("         { return '('; }
")"         { return ')'; }
";"         { return ';'; }
"*"         { return '*'; }
"%"         { return '%'; } 


[a-zA-Z_][a-zA-Z0-9_]* {
    yylval.str = strdup(yytext);
    return TOK_IDENT;
}

[0-9]+ {
    yylval.num = atoi(yytext);
    return TOK_NUMBER;
}

'[^']*' {
    yylval.str = strdup(yytext + 1);
    yylval.str[strlen(yylval.str) - 1] = '\0';
    return TOK_STRING;
}

[ \t\n\r]+ { /* 忽略空格 */ }

. {
    fprintf(stderr, "Unknown character: %c\n", *yytext);
}

%%