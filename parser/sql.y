%code requires {
#include "ast.h"

/* data_type 的语义值：类型 + 可选的显式长度（VARCHAR(n) / CHAR(n)） */
typedef struct CTypeSpec {
    CDataType type;
    unsigned length;     /* 0 = 未声明 */
    int has_length;      /* 是否显式写了 (n) */
} CTypeSpec;
}

%{
#include "ast.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern int yylex();
void yyerror(const char *s);

/* LIMIT/OFFSET 的 AST 字段是 int，这里做一次显式的范围检查 */
static int sql_limit_to_int(int64_t value, int *out) {
    if (value < 0 || value > 2147483647LL) {
        return 0;
    }
    *out = (int)value;
    return 1;
}

%}

%debug
%verbose
%define parse.error detailed
/* 打开位置跟踪：语法动作里可以用 @$/@1 拿到 token/规则的起止行列 */
%locations


/* yyltype, yylval */
%union {
    int num;
    int64_t i64;
    char* str;
    COpType op;
    CDataType dt;
    CTypeSpec spec;
    struct ASTNode* node;
    struct ASTNode* list;
}

%token TOK_USE TOK_INSERT TOK_DELETE TOK_UPDATE TOK_SET TOK_INTO TOK_VALUES 
%token TOK_SELECT TOK_WHERE TOK_FROM 
%token TOK_AND TOK_OR TOK_NOT 
%token TOK_GE TOK_LE TOK_GT TOK_LT
%token TOK_EQ TOK_NE TOK_IN TOK_IS TOK_LIKE
%token TOK_ORDER TOK_BY TOK_ASC TOK_DESC TOK_LIMIT TOK_OFFSET

/* DDL */
%token TOK_CREATE TOK_DROP TOK_DATABASE TOK_TABLE
%token TOK_PRIMARY TOK_KEY
/* 事务控制 */
%token TOK_BEGIN TOK_COMMIT TOK_ROLLBACK TOK_START TOK_TRANSACTION TOK_WORK
%token TOK_END TOK_ABORT
/* 语句前缀 */
%token TOK_EXPLAIN TOK_ANALYZE
%token TOK_NULL TOK_TRUE TOK_FALSE
%token TOK_LEX_ERROR

%token <i64> TOK_NUMBER
%token <str> TOK_IDENT TOK_STRING TOK_TYPE_NAME

/* DML */
%type <node> input statement select_stmt insert_stmt update_stmt delete_stmt
%type <list> select_list column_name_list
%type <list> insert_columns insert_values in_values
%type <list> value_item_list
%type <node> value_item 
%type <list> assignment_list
%type <node> assignment

%type <node> opt_where condition_expr condition_term condition_factor
%type <op> compare_op

%type <node> opt_limit opt_order_by order_item 
%type <list> order_list 
%type <op> order_direction

/* DDL */
%type <node> use_stmt create_db_stmt drop_db_stmt create_table_stmt drop_table_stmt
%type <list> column_def_list
%type <node> column_def
%type <spec> data_type 
%type <num> opt_nullable opt_primary_key

/* 事务控制 */
%type <node> transaction_stmt

/* 语句前缀 */
%type <node> explain_stmt


%left TOK_OR
%left TOK_AND
%right TOK_NOT
%nonassoc TOK_IS
%nonassoc TOK_EQ TOK_LT TOK_GT TOK_GE TOK_LE TOK_NE TOK_IN TOK_LIKE

%%


input:
    statement ';'     { set_parsed_ast($1); }
    | explain_stmt ';' { set_parsed_ast($1); }
    ;

statement:
    use_stmt          { $$ = $1; }
    | select_stmt     { $$ = $1; }
    | insert_stmt     { $$ = $1; }
    | update_stmt     { $$ = $1; }
    | delete_stmt     { $$ = $1; }
    | create_db_stmt  { $$ = $1; }
    | drop_db_stmt    { $$ = $1; }
    | create_table_stmt { $$ = $1; }
    | drop_table_stmt { $$ = $1; }
    | transaction_stmt { $$ = $1; }
    ;


/* ============================================================
   DDL: DATABASE
   ============================================================ */
use_stmt:
    TOK_USE TOK_IDENT {
        $$ = make_use_node($2);
        AST_SET_SPAN($$, @$);
        ((DatabaseNode*)$$->data)->db_span = sspan_make(@2.first_line, @2.first_column, @2.last_line, @2.last_column);
    }
    ;

create_db_stmt:
    TOK_CREATE TOK_DATABASE TOK_IDENT {
        $$ = make_create_database_node($3);
        AST_SET_SPAN($$, @$);
        ((DatabaseNode*)$$->data)->db_span = sspan_make(@3.first_line, @3.first_column, @3.last_line, @3.last_column);
    }
    ;

drop_db_stmt:
    TOK_DROP TOK_DATABASE TOK_IDENT {
        $$ = make_drop_database_node($3);
        AST_SET_SPAN($$, @$);
        ((DatabaseNode*)$$->data)->db_span = sspan_make(@3.first_line, @3.first_column, @3.last_line, @3.last_column);
    }
    ;

/* ============================================================
   DDL: TABLE
   ============================================================ */
create_table_stmt:
    TOK_CREATE TOK_TABLE TOK_IDENT '(' column_def_list ')' {
        $$ = make_create_table_node($3, $5);
        AST_SET_SPAN($$, @$);
        ((CreateTableNode*)$$->data)->table_span = sspan_make(@3.first_line, @3.first_column, @3.last_line, @3.last_column);
    }
    ;

drop_table_stmt:
    TOK_DROP TOK_TABLE TOK_IDENT {
        $$ = make_drop_table_node($3);
        AST_SET_SPAN($$, @$);
        ((DropTableNode*)$$->data)->table_span = sspan_make(@3.first_line, @3.first_column, @3.last_line, @3.last_column);
    }
    ;

/* ============================================================
   列定义
   ============================================================ */
column_def_list:
    column_def                         { $$ = create_list($1, LIST_COLUMN_DEF); }
    | column_def_list ',' column_def   { $$ = append_to_list($1, $3); }
    ;

column_def:
    TOK_IDENT data_type opt_primary_key opt_nullable {
        $$ = make_column_def_node($1, $2.type, $2.length, $3, $4);
        AST_SET_SPAN($$, @$);
    }
    ;

/* 类型：名字来自 common/c_types.h 的别名表（与 sql_types 共用一份定义），
   长度校验也用同一份规则（CHAR <= 255 / VARCHAR <= 65535 / 不接受长度的类型报错）。 */
data_type:
    TOK_TYPE_NAME {
        CDataType type = c_type_lookup($1);
        if (type == DT_UNKNOWN) {
            yyerror("unknown data type");
            free($1);
            YYERROR;
        }
        $$.type = type;
        $$.length = 0;
        $$.has_length = 0;
        free($1);
    }
    | TOK_TYPE_NAME '(' TOK_NUMBER ')' {
        CDataType type = c_type_lookup($1);
        if (type == DT_UNKNOWN) {
            yyerror("unknown data type");
            free($1);
            YYERROR;
        }
        if ($3 <= 0 || $3 > (int64_t)c_type_max_length(type) ||
            !c_type_accepts_length(type)) {
            if (c_type_accepts_length(type)) {
                yyerror("declared length out of range for this type");
            } else {
                yyerror("this data type does not accept a length");
            }
            free($1);
            YYERROR;
        }
        $$.type = type;
        $$.length = (unsigned)$3;
        $$.has_length = 1;
        free($1);
    }
    ;

opt_primary_key:
    %empty                { $$ = 0; }
    | TOK_PRIMARY TOK_KEY { $$ = 1; }
    ;

opt_nullable:
    %empty                { $$ = 1; }   /* 默认可为空 */
    | TOK_NOT TOK_NULL    { $$ = 0; }   /* NOT NULL */
    | TOK_NULL            { $$ = 1; }   /* 显式 NULL */
    ;

/* ============================================================
   EXPLAIN [ANALYZE] <语句>

   只是给语句加一层"不要执行、给我计划"的前缀：包成一个 NODE_EXPLAIN
   节点，被解释的语句还是原来的节点类型（session 拆掉这层后照常走
   builder/validator/planner）。EXPLAIN 的输出不是查询结果，所以
   sql::Query 里没有它。
   ============================================================ */
explain_stmt:
    TOK_EXPLAIN statement {
        $$ = make_explain_node(0, $2);
        AST_SET_SPAN($$, @$);
    }
    | TOK_EXPLAIN TOK_ANALYZE statement {
        $$ = make_explain_node(1, $3);
        AST_SET_SPAN($$, @$);
    }
    ;

/* ============================================================
   事务控制：BEGIN / COMMIT / ROLLBACK

   语法层只负责"认出这是哪一种事务操作"，执行（开事务/提交/回滚、
   "事务已中止"之类的会话状态）在 session 层，因为它需要连接级状态。
   这里同时给出标准别名与明确报错的未支持形式（比 "syntax error" 有用）。
   ============================================================ */
transaction_stmt:
    TOK_BEGIN                    { $$ = make_transaction_node(TXN_BEGIN);
                                   AST_SET_SPAN($$, @$); }
    | TOK_BEGIN TOK_WORK         { $$ = make_transaction_node(TXN_BEGIN);
                                   AST_SET_SPAN($$, @$); }
    | TOK_BEGIN TOK_TRANSACTION  { $$ = make_transaction_node(TXN_BEGIN);
                                   AST_SET_SPAN($$, @$); }
    | TOK_START TOK_TRANSACTION  { $$ = make_transaction_node(TXN_BEGIN);
                                   AST_SET_SPAN($$, @$); }
    | TOK_COMMIT                 { $$ = make_transaction_node(TXN_COMMIT);
                                   AST_SET_SPAN($$, @$); }
    | TOK_COMMIT TOK_WORK        { $$ = make_transaction_node(TXN_COMMIT);
                                   AST_SET_SPAN($$, @$); }
    | TOK_END                    { $$ = make_transaction_node(TXN_COMMIT);
                                   AST_SET_SPAN($$, @$); }
    | TOK_END TOK_WORK           { $$ = make_transaction_node(TXN_COMMIT);
                                   AST_SET_SPAN($$, @$); }
    | TOK_ROLLBACK               { $$ = make_transaction_node(TXN_ROLLBACK);
                                   AST_SET_SPAN($$, @$); }
    | TOK_ROLLBACK TOK_WORK      { $$ = make_transaction_node(TXN_ROLLBACK);
                                   AST_SET_SPAN($$, @$); }
    | TOK_ABORT                  { $$ = make_transaction_node(TXN_ROLLBACK);
                                   AST_SET_SPAN($$, @$); }
    | TOK_ABORT TOK_WORK         { $$ = make_transaction_node(TXN_ROLLBACK);
                                   AST_SET_SPAN($$, @$); }
    /* ---- 明确报错的未支持形式 ---- */
    | TOK_BEGIN TOK_IDENT {
        yyerror("transaction modes are not supported (use plain BEGIN)");
        YYERROR;
    }
    | TOK_START TOK_TRANSACTION TOK_IDENT {
        yyerror("transaction modes are not supported (use START TRANSACTION)");
        YYERROR;
    }
    | TOK_COMMIT TOK_AND TOK_IDENT {
        yyerror("COMMIT AND CHAIN/NO CHAIN is not supported");
        YYERROR;
    }
    | TOK_ROLLBACK TOK_AND TOK_IDENT {
        yyerror("ROLLBACK AND CHAIN/NO CHAIN is not supported");
        YYERROR;
    }
    | TOK_ROLLBACK TOK_IDENT {
        /* 只可能是 ROLLBACK TO [SAVEPOINT] name —— 我们没有保存点 */
        yyerror("ROLLBACK TO SAVEPOINT is not supported");
        YYERROR;
    }
    ;

/* ============================================================
   SELECT 语句
   ============================================================ */
select_stmt:
    TOK_SELECT select_list TOK_FROM TOK_IDENT opt_where opt_order_by opt_limit
    {
        $$ = make_select_node($4, $2, $5, $6, $7);
        AST_SET_SPAN($$, @$);
        ((SelectNode*)$$->data)->table_span = sspan_make(@4.first_line, @4.first_column, @4.last_line, @4.last_column);
    }
    ;

select_list:
    '*'                          {
        ASTNode* item = make_ident_node("*");
        AST_SET_SPAN(item, @1);
        $$ = create_list(item, LIST_COLUMN);
    }
    | column_name_list           { $$ = $1; }
    ;

/* ============================================================
   列名列表
   ============================================================ */
column_name_list:
    TOK_IDENT {
        ASTNode* item = make_ident_node($1);
        AST_SET_SPAN(item, @1);
        $$ = create_list(item, LIST_COLUMN);
    }
    | column_name_list ',' TOK_IDENT {
        ASTNode* item = make_ident_node($3);
        AST_SET_SPAN(item, @3);
        $$ = append_to_list($1, item);
    }
    ;


/* ============================================================
   INSERT / UPDATE / DELETE
   ============================================================ */
insert_stmt:
    TOK_INSERT TOK_INTO TOK_IDENT insert_columns TOK_VALUES insert_values
    {
        $$ = make_insert_node($3, $4, $6);
        AST_SET_SPAN($$, @$);
        ((InsertNode*)$$->data)->table_span = sspan_make(@3.first_line, @3.first_column, @3.last_line, @3.last_column);
    }
    ;

insert_columns:
    %empty                         { $$ = NULL; }
    | '(' column_name_list ')'     { $$ = $2; }
    ;

/* VALUES 的语法是"一个或多个行"，AST 里统一是**行的列表**：
   insert_values 的每个元素都是一个 LIST_VALUE（那一行的值列表）。
   这样单行与多行的形状一致，builder 不需要分两套。
   （用左递归就是为了让单行/多行没有歧义：')' 之后看到 ',' 就继续收集行。） */
insert_values:
    '(' value_item_list ')'                     { $$ = create_list($2, LIST_VALUE); }
    | insert_values ',' '(' value_item_list ')' { $$ = append_to_list($1, $4); }
    ;

in_values:
    '(' value_item_list ')'      { $$ = $2; }
    ;

value_item_list:
    value_item                   { $$ = create_list($1, LIST_VALUE); }
    | value_item_list ',' value_item { $$ = append_to_list($1, $3); }
    ;

value_item:
    TOK_NUMBER                   { $$ = make_number_node($1); AST_SET_SPAN($$, @$); }
    | '-' TOK_NUMBER             { $$ = make_number_node(-$2); AST_SET_SPAN($$, @$); }
    | TOK_STRING                 { $$ = make_string_node($1); AST_SET_SPAN($$, @$); }
    | TOK_NULL                   { $$ = make_literal_node(LITERAL_NULL); AST_SET_SPAN($$, @$); }
    | TOK_TRUE                   { $$ = make_literal_node(LITERAL_TRUE); AST_SET_SPAN($$, @$); }
    | TOK_FALSE                  { $$ = make_literal_node(LITERAL_FALSE); AST_SET_SPAN($$, @$); }
    ;


update_stmt:
    TOK_UPDATE TOK_IDENT TOK_SET assignment_list opt_where
    {
        $$ = make_update_node($2, $4, $5);
        AST_SET_SPAN($$, @$);
        ((UpdateNode*)$$->data)->table_span = sspan_make(@2.first_line, @2.first_column, @2.last_line, @2.last_column);
    }
    ;

assignment_list:
    assignment                   { $$ = create_list($1,LIST_ASSIGNMENT); }
    | assignment_list ',' assignment { $$ = append_to_list($1, $3); }
    ;

assignment:
    TOK_IDENT TOK_EQ value_item    {
        $$ = make_assignment_node($1, $3);
        AST_SET_SPAN($$, @$);
    }
    ;


delete_stmt:
    TOK_DELETE TOK_FROM TOK_IDENT opt_where
    {
        $$ = make_delete_node($3, $4);
        AST_SET_SPAN($$, @$);
        ((DeleteNode*)$$->data)->table_span = sspan_make(@3.first_line, @3.first_column, @3.last_line, @3.last_column);
    }
    ;

/* ============================================================
   WHERE 条件
   ============================================================ */
opt_where:
    %empty                           { $$ = NULL; }
    | TOK_WHERE condition_expr       { $$ = $2; }
    ;

condition_expr:
    condition_term               { $$ = $1; }
    | condition_expr TOK_OR condition_term {
        $$ = make_binary_node($1, OP_OR, $3);
        AST_SET_SPAN($$, @$);
    }
    ;

condition_term:
    condition_factor             { $$ = $1; }
    | condition_term TOK_AND condition_factor {
        $$ = make_binary_node($1, OP_AND, $3);
        AST_SET_SPAN($$, @$);
    }
    ;

condition_factor:
    TOK_IDENT compare_op value_item {
        $$ = make_compare_node($1, $2, $3);
        AST_SET_SPAN($$, @$);
    }
    | TOK_IDENT TOK_IN in_values {
        $$ = make_in_node($1, $3);
        AST_SET_SPAN($$, @$);
    }
    | TOK_IDENT TOK_NOT TOK_IN in_values {
        $$ = make_not_node(make_in_node($1, $4));
        AST_SET_SPAN($$, @$);
    }
    | TOK_IDENT TOK_LIKE TOK_STRING {
        $$ = make_compare_node($1, OP_LIKE, make_string_node($3));
        AST_SET_SPAN($$, @$);
    }
    | TOK_IDENT TOK_NOT TOK_LIKE TOK_STRING {
        $$ = make_not_node(make_compare_node($1, OP_LIKE, make_string_node($4)));
        AST_SET_SPAN($$, @$);
    }
    | TOK_IDENT TOK_IS TOK_NULL {
        $$ = make_compare_node($1, OP_IS_NULL, NULL);
        AST_SET_SPAN($$, @$);
    }
    | TOK_IDENT TOK_IS TOK_NOT TOK_NULL {
        $$ = make_compare_node($1, OP_IS_NOT_NULL, NULL);
        AST_SET_SPAN($$, @$);
    }
    | '(' condition_expr ')' { $$ = $2; }
    | TOK_NOT condition_factor {
        $$ = make_not_node($2);
        AST_SET_SPAN($$, @1);
    }
    ;

/* ============================================================
   比较操作符
   ============================================================ */
compare_op:
    TOK_EQ     { $$ = OP_EQ; }
    | TOK_NE   { $$ = OP_NE; }
    | TOK_GT   { $$ = OP_GT; }
    | TOK_GE   { $$ = OP_GE; }
    | TOK_LT   { $$ = OP_LT; }
    | TOK_LE   { $$ = OP_LE; }
    ;

/* ============================================================
   ORDER BY
   ============================================================ */
opt_order_by:
    %empty                       { $$ = NULL; }
    | TOK_ORDER TOK_BY order_list { $$ = $3; }
    ;

order_list:
    order_item                  { $$ = create_list($1, LIST_ORDER);}
    | order_list ',' order_item    { $$ = append_to_list($1, $3); }
    ;

order_item:
    TOK_IDENT                   {
        $$ = make_order_node($1, OP_ASC);
        AST_SET_SPAN($$, @$);
    }
    | TOK_IDENT order_direction {
        $$ = make_order_node($1, $2);
        AST_SET_SPAN($$, @$);
    }
    ;

order_direction:
    TOK_ASC    { $$ = OP_ASC; }
    | TOK_DESC { $$ = OP_DESC; }
    ;
    
/* ============================================================
   LIMIT / OFFSET
   ============================================================ */
opt_limit:
    %empty                       { $$ = NULL; }
    | TOK_LIMIT TOK_NUMBER {
        int limit_value = 0;
        if (!sql_limit_to_int($2, &limit_value)) {
            yyerror("LIMIT out of range");
            YYERROR;
        }
        $$ = make_limit_node(limit_value, 1, 0, 0);
    }
    | TOK_LIMIT TOK_NUMBER TOK_OFFSET TOK_NUMBER {
        int limit_value = 0;
        int offset_value = 0;
        if (!sql_limit_to_int($2, &limit_value) ||
            !sql_limit_to_int($4, &offset_value)) {
            yyerror("LIMIT/OFFSET out of range");
            YYERROR;
        }
        $$ = make_limit_node(limit_value, 1, offset_value, 1);
    }
    | TOK_LIMIT TOK_NUMBER ',' TOK_NUMBER {  /* MySQL 风格: LIMIT offset, count */
        int limit_value = 0;
        int offset_value = 0;
        if (!sql_limit_to_int($4, &limit_value) ||
            !sql_limit_to_int($2, &offset_value)) {
            yyerror("LIMIT out of range");
            YYERROR;
        }
        $$ = make_limit_node(limit_value, 1, offset_value, 1);
    }
    ;

%%
