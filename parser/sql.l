%{
#include "parser.tab.h"
#include <cstring>
#include <stdio.h>
#include <stdlib.h>

void debug_token(const char* name, int token) {
    printf("DEBUG: %s -> token %d\n", name, token);
}
%}

%option noyywrap
%option case-insensitive

%%

"select"    { printf("DEBUG: matched 'select' -> TOK_SELECT (%d)\n", TOK_SELECT); return TOK_SELECT; }
"from"      { printf("DEBUG: matched 'from' -> TOK_FROM (%d)\n", TOK_FROM); return TOK_FROM; }
"where"     { printf("DEBUG: matched 'where' -> TOK_WHERE (%d)\n", TOK_WHERE); return TOK_WHERE; }
"and"       { printf("DEBUG: matched 'and' -> TOK_AND (%d)\n", TOK_AND); return TOK_AND; }
"or"        { printf("DEBUG: matched 'or' -> TOK_OR (%d)\n", TOK_OR); return TOK_OR; }
"insert"    { printf("DEBUG: matched 'insert' -> TOK_INSERT (%d)\n", TOK_INSERT); return TOK_INSERT; }
"into"      { printf("DEBUG: matched 'into' -> TOK_INTO (%d)\n", TOK_INTO); return TOK_INTO; }
"values"    { printf("DEBUG: matched 'values' -> TOK_VALUES (%d)\n", TOK_VALUES); return TOK_VALUES; }
"update"    { printf("DEBUG: matched 'update' -> TOK_UPDATE (%d)\n", TOK_UPDATE); return TOK_UPDATE; }
"set"       { printf("DEBUG: matched 'set' -> TOK_SET (%d)\n", TOK_SET); return TOK_SET; }
"delete"    { printf("DEBUG: matched 'delete' -> TOK_DELETE (%d)\n", TOK_DELETE); return TOK_DELETE; }
"use"       { printf("DEBUG: matched 'use' -> TOK_USE (%d)\n", TOK_USE); return TOK_USE; }

">="        { printf("DEBUG: matched '>=' -> TOK_GE (%d)\n", TOK_GE); return TOK_GE; }
"<="        { printf("DEBUG: matched '<=' -> TOK_LE (%d)\n", TOK_LE); return TOK_LE; }
">"         { printf("DEBUG: matched '>' -> '%c' (%d)\n", *yytext, *yytext); return '>'; }
"<"         { printf("DEBUG: matched '<' -> '%c' (%d)\n", *yytext, *yytext); return '<'; }
"="         { printf("DEBUG: matched '=' -> '%c' (%d)\n", *yytext, *yytext); return '='; }

","         { printf("DEBUG: matched ',' -> '%c' (%d)\n", *yytext, *yytext); return ','; }
"("         { printf("DEBUG: matched '(' -> '%c' (%d)\n", *yytext, *yytext); return '('; }
")"         { printf("DEBUG: matched ')' -> '%c' (%d)\n", *yytext, *yytext); return ')'; }
";"         { printf("DEBUG: matched ';' -> '%c' (%d)\n", *yytext, *yytext); return ';'; }
"*"         { printf("DEBUG: matched '*' -> '%c' (%d)\n", *yytext, *yytext); return '*'; }

[a-zA-Z_][a-zA-Z0-9_]* {
    printf("DEBUG: matched IDENT: '%s' -> TOK_IDENT (%d)\n", yytext, TOK_IDENT);
    yylval.str = strdup(yytext);
    return TOK_IDENT;
}

[0-9]+ {
    printf("DEBUG: matched NUMBER: '%s' -> TOK_NUMBER (%d)\n", yytext, TOK_NUMBER);
    yylval.num = atoi(yytext);
    return TOK_NUMBER;
}

'[^']*' {
    printf("DEBUG: matched STRING: '%s' -> TOK_STRING (%d)\n", yytext, TOK_STRING);
    yylval.str = strdup(yytext + 1);
    yylval.str[strlen(yylval.str) - 1] = '\0';
    return TOK_STRING;
}

[ \t\n\r]+ { printf("DEBUG: whitespace\n"); }

. {
    printf("DEBUG: Unknown character: '%c' (ASCII %d)\n", *yytext, *yytext);
    fprintf(stderr, "Unknown character: %c\n", *yytext);
}

%%