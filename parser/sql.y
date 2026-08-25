%{
#include "ast.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// 声明 flex 的词法分析函数
extern int yylex();

// 声明错误处理函数
void yyerror(const char *s);

%}

%debug
%verbose

/* 定义优先级（从低到高） */
%left TOK_OR
%left TOK_AND
%nonassoc '=' '<' '>' TOK_GE TOK_LE

%union {
    int num;
    char* str;
    struct ASTNode* node;
    struct ASTNodeList* list; /* 可选：如果你用链表存列名/值列表 */
}

%token TOK_INTO TOK_VALUES TOK_SET

%token TOK_USE TOK_SELECT TOK_INSERT TOK_UPDATE TOK_DELETE

%token TOK_WHERE TOK_FROM TOK_AND TOK_OR TOK_GE TOK_LE

%token <num> TOK_NUMBER

%token <str> TOK_IDENT TOK_STRING

/* 非终结符类型声明 */
%type <node> input statement use_stmt select_stmt insert_stmt update_stmt delete_stmt
%type <node> opt_where condition_expr condition_term condition_factor
%type <node> assignment value_item
%type <list> select_list column_list value_list column_name_list value_item_list assignment_list
%%

input:
    statement ';'     { set_parsed_ast($1); }
    ;

statement:
    use_stmt          { $$ = $1; }
    | select_stmt     { $$ = $1; }
    | insert_stmt     { $$ = $1; }
    | update_stmt     { $$ = $1; }
    | delete_stmt     { $$ = $1; }
    ;

use_stmt:
    TOK_USE TOK_IDENT { $$ = make_use_node($2); }
    ;

select_stmt:
    TOK_SELECT select_list TOK_FROM TOK_IDENT opt_where
    {
        $$ = make_select_node($4, $2, $5); // table, column_list, condition
    }
    ;

select_list:
    '*'                          { $$ = create_list(make_ident_node("*")); }
    | column_name_list          { $$ = $1; }
    ;

insert_stmt:
    TOK_INSERT TOK_INTO TOK_IDENT column_list TOK_VALUES value_list
    {
        // 假设 make_insert_node 签名为: (table, columns_node, values_node)
        $$ = make_insert_node($3, $4, $6);
    }
    ;



update_stmt:
    TOK_UPDATE TOK_IDENT TOK_SET assignment_list opt_where
    {
        // 假设 make_update_node 签名为: (table, assignments, condition)
        // 如果 opt_where 为空，传 NULL
        $$ = make_update_node($2, $4, $5);
    }
    ;



delete_stmt:
    TOK_DELETE TOK_FROM TOK_IDENT opt_where
    {
        $$ = make_delete_node($3, $4); // table, condition
    }
    ;

/* --- 辅助规则 --- */
opt_where:
    %empty                           { $$ = NULL; }

    | TOK_WHERE condition_expr       { $$ = $2; }
    ;

condition_expr:
    condition_term               { $$ = $1; }
    | condition_expr TOK_OR condition_term { $$ = make_binary_node($1, "OR", $3); }
    ;

condition_term:
    condition_factor             { $$ = $1; }
    | condition_term TOK_AND condition_factor { $$ = make_binary_node( $1, "AND", $3); }
    ;

condition_factor:
    TOK_IDENT '=' TOK_NUMBER     { $$ = make_compare_node(make_ident_node($1), "=", make_number_node($3)); }
    | TOK_IDENT '>' TOK_NUMBER   { $$ = make_compare_node(make_ident_node($1), ">", make_number_node($3)); }
    | TOK_IDENT '<' TOK_NUMBER   { $$ = make_compare_node(make_ident_node($1), "<", make_number_node($3)); }
    | TOK_IDENT TOK_GE TOK_NUMBER { $$ = make_compare_node(make_ident_node($1), ">=", make_number_node($3)); }
    | TOK_IDENT TOK_LE TOK_NUMBER { $$ = make_compare_node(make_ident_node($1), "<=", make_number_node($3)); }
    | TOK_IDENT '=' TOK_STRING   { $$ = make_compare_node(make_ident_node($1), "=", make_string_node($3)); }
    | '(' condition_expr ')'     { $$ = $2; }  /* ← 支持括号分组 */
    ;

column_name_list:
    TOK_IDENT                    { $$ = create_list(make_ident_node($1)); }
    | column_name_list ',' TOK_IDENT { $$ = append_to_list($1, make_ident_node($3)); }
    ;

/* 列名列表 (id, name) -> 构建一个 AST 节点或列表 */
column_list:
    '(' column_name_list ')'        { $$ = $2; }
    ;

/* 值列表 (1, 'abc') -> 构建一个 AST 节点或列表 */
value_list:
    '(' value_item_list ')'               { $$ = $2; }

value_item_list:
    value_item                       { $$ = create_list($1); }
    | value_item_list ',' value_item { $$ = append_to_list($1, $3); }
    ;

value_item:
    TOK_NUMBER                       { $$ = make_number_node($1); }

    | TOK_STRING                     { $$ = make_string_node($1); }
    ;

assignment_list:
    assignment                     { $$ = create_list($1); }

    | assignment_list ',' assignment { $$ = append_to_list($1, $3); }
    ;

assignment:
    TOK_IDENT '=' TOK_NUMBER       { $$ = make_assignment_node(make_ident_node($1), make_number_node($3)); }

    | TOK_IDENT '=' TOK_STRING     { $$ = make_assignment_node(make_ident_node($1), make_string_node($3)); }
    ;



%%

void yyerror(const char *s) {
    extern int yylineno;  // 如果启用了行号
    fprintf(stderr, "Parse error at line %d: %s\n", yylineno, s);


    extern int yychar;
    extern char* yytext;

    fprintf(stderr, "Current token: %d\n", yychar);

    // 尝试显示 token 名称
    if (yychar < 256) {
        fprintf(stderr, "Token character: '%c' (ASCII %d)\n", yychar, yychar);
    } else {
        fprintf(stderr, "Token number: %d\n", yychar);
    }
}
