// ast.h
#ifndef AST_H
#define AST_H

#include "common/c_types.h"
#include "common/source_span.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ============ AST 节点类型枚举 ============ */
typedef enum {
  NODE_NUMBER,
  NODE_STRING,
  NODE_IDENT,
  NODE_LITERAL, /* NULL / TRUE / FALSE 关键字字面量 */

  NODE_USE,
  NODE_SELECT,
  NODE_INSERT,
  NODE_UPDATE,
  NODE_DELETE,

  NODE_COMPARE,
  NODE_BINARY_OP,
  NODE_ASSIGNMENT,
  NODE_LIST,
  NODE_NOT,
  NODE_IN,
  NODE_NOT_IN,
  NODE_ORDER,
  NODE_LIMIT,
  /* DDL */
  NODE_CREATE_DATABASE,
  NODE_DROP_DATABASE,
  NODE_CREATE_TABLE,
  NODE_DROP_TABLE,
  NODE_COLUMN_DEF,
  NODE_TYPE_COUNT
} NodeType;

static inline const char *node_type_to_string(NodeType type) {
  switch (type) {
  case NODE_NUMBER:
    return "NUMBER";
  case NODE_STRING:
    return "STRING";
  case NODE_IDENT:
    return "IDENT";
  case NODE_LITERAL:
    return "LITERAL";
  case NODE_USE:
    return "USE";
  case NODE_SELECT:
    return "SELECT";
  case NODE_INSERT:
    return "INSERT";
  case NODE_UPDATE:
    return "UPDATE";
  case NODE_DELETE:
    return "DELETE";
  case NODE_COMPARE:
    return "COMPARE";
  case NODE_BINARY_OP:
    return "BINARY_OP";
  case NODE_ASSIGNMENT:
    return "ASSIGNMENT";
  case NODE_LIST:
    return "LIST";
  case NODE_NOT:
    return "NOT";
  case NODE_IN:
    return "IN";
  case NODE_NOT_IN:
    return "NOT_IN";
  case NODE_ORDER:
    return "ORDER";
  case NODE_LIMIT:
    return "LIMIT";
  case NODE_CREATE_DATABASE:
    return "CREATE_DATABASE";
  case NODE_DROP_DATABASE:
    return "DROP_DATABASE";
  case NODE_CREATE_TABLE:
    return "CREATE_TABLE";
  case NODE_DROP_TABLE:
    return "DROP_TABLE";
  case NODE_COLUMN_DEF:
    return "COLUMN_DEF";
  default:
    return "UNKNOWN_NODE_TYPE";
  }
}

// 前向声明，因为结构体内部会互相引用
typedef struct ASTNode ASTNode;
typedef struct ASTNodeList ASTNodeList;

typedef ASTNode *(*NodeDataConstructor)(void)__attribute__((__may_alias__));
typedef void (*NodeDataDestructor)(ASTNode *node);

typedef int (*NodeDataPrinter)(ASTNode *node, int indent, int offset,
                               char *buffer, size_t buffer_size);
typedef struct NodeLifetime {
  NodeType type;
  NodeDataConstructor do_create;
  NodeDataDestructor do_free;
  NodeDataPrinter to_string;
} NodeLifetime;

NodeLifetime *get_node_lifetime(NodeType type);

// NODE_NUMBER
typedef struct NumberNode {
  int64_t value;
} NumberNode;
ASTNode *make_number_node(int64_t num);
int print_number_node(ASTNode *node, int indent, int offset, char *buffer,
                      size_t buffer_size);
void free_number_node(ASTNode *node);

// NODE_LITERAL: NULL / TRUE / FALSE
typedef enum { LITERAL_NULL, LITERAL_TRUE, LITERAL_FALSE } LiteralKind;

typedef struct LiteralNode {
  LiteralKind kind;
} LiteralNode;
ASTNode *make_literal_node(LiteralKind kind);
const char *literal_kind_to_string(LiteralKind kind);
int print_literal_node(ASTNode *node, int indent, int offset, char *buffer,
                       size_t buffer_size);
void free_literal_node(ASTNode *node);

// NODE_STRING
typedef struct StringNode {
  char *value;
} StringNode;
ASTNode *make_string_node(const char *str);
int print_string_node(ASTNode *node, int indent, int offset, char *buffer,
                      size_t buffer_size);
void free_string_node(ASTNode *node);

// NODE_IDENT
typedef struct IdentNode {
  char *name;
} IdentNode;
ASTNode *make_ident_node(const char *name);
int print_ident_node(ASTNode *node, int indent, int offset, char *buffer,
                     size_t buffer_size);
void free_ident_node(ASTNode *node);

/* ============ SQL 语句节点 ============ */
// NODE_SELECT
typedef struct SelectNode {
  char *table;        // 表名
  SSpan table_span;   // 表名在 SQL 文本中的位置（供报错定位）
  ASTNode *columns;   // 列名列表 (ASTNodeList)
  ASTNode *condition; // WHERE 条件 (ASTNode)
  ASTNode *order_by;  // ORDER BY 节点 (ASTNode)
  ASTNode *limit;     // LIMIT 节点 (ASTNode)

} SelectNode;

ASTNode *make_select_node(const char *table, ASTNode *columns,
                          ASTNode *condition, ASTNode *order_by,
                          ASTNode *limit);
int print_select_node(ASTNode *node, int indent, int offset, char *buffer,
                      size_t buffer_size);
void free_select_node(ASTNode *node);

// NODE_INSERT
typedef struct InsertNode {
  char *table;      // 表名
  SSpan table_span; // 表名位置
  ASTNode *columns; // 列名列表 (ASTNodeList)
  ASTNode *values;  // 值列表 (ASTNodeList)
} InsertNode;
ASTNode *make_insert_node(const char *table, ASTNode *columns, ASTNode *values);
int print_insert_node(ASTNode *node, int indent, int offset, char *buffer,
                      size_t buffer_size);
void free_insert_node(ASTNode *node);

// NODE_UPDATE
typedef struct UpdateNode {
  char *table;          // 表名
  SSpan table_span;     // 表名位置
  ASTNode *assignments; // 列名=值列表 (ASTNodeList)
  ASTNode *condition;   // WHERE 条件 (ASTNode)
} UpdateNode;

ASTNode *make_update_node(const char *table, ASTNode *assignments,
                          ASTNode *condition);
int print_update_node(ASTNode *node, int indent, int offset, char *buffer,
                      size_t buffer_size);
void free_update_node(ASTNode *node);

// NODE_ASSIGNMENT
typedef struct AssignmentNode {
  char *column;   // 列名
  ASTNode *value; // 值 (ASTNode)
} AssignmentNode;
ASTNode *make_assignment_node(const char *column, ASTNode *value);
int print_assignment_node(ASTNode *node, int indent, int offset, char *buffer,
                          size_t buffer_size);
void free_assignment_node(ASTNode *node);

// NODE_DELETE
typedef struct DeleteNode {
  char *table;        // 表名
  SSpan table_span;   // 表名位置
  ASTNode *condition; // WHERE 条件 (ASTNode)
} DeleteNode;
ASTNode *make_delete_node(const char *table, ASTNode *condition);
int print_delete_node(ASTNode *node, int indent, int offset, char *buffer,
                      size_t buffer_size);
void free_delete_node(ASTNode *node);

/* ============ 条件表达式叶子节点 ============ */
// NODE_COMPARE
typedef struct CompareNode {
  char *column;   // 列名
  COpType op;     // 比较操作符
  ASTNode *right; // 右侧值 (ASTNode)
} CompareNode;

ASTNode *make_compare_node(const char *column, COpType optype, ASTNode *right);
int print_compare_node(ASTNode *node, int indent, int offset, char *buffer,
                       size_t buffer_size);
void free_compare_node(ASTNode *node);

// NODE_IN
typedef struct InNode {
  char *column;    // 列名
  ASTNode *values; // 值列表 (ASTNodeList)
  char excluded;
} InNode;
ASTNode *make_in_node(const char *column, ASTNode *values);
ASTNode *make_not_in_node(const char *column, ASTNode *values);
int print_in_node(ASTNode *node, int indent, int offset, char *buffer,
                  size_t buffer_size);
void free_in_node(ASTNode *node);

/* ============ 条件表达式非叶子节点 ============ */
// NODE_BINARY_OP
typedef struct BinaryOpNode {
  ASTNode *left;  // 左侧表达式 (ASTNode)
  COpType op;     // 二元操作符
  ASTNode *right; // 右侧表达式 (ASTNode)
} BinaryOpNode;

ASTNode *make_binary_node(ASTNode *left, COpType optype, ASTNode *right);
int print_binary_node(ASTNode *node, int indent, int offset, char *buffer,
                      size_t buffer_size);
void free_binary_node(ASTNode *node);

// NODE_NOT
typedef struct NotNode {
  ASTNode *child; // 子表达式 (ASTNode)
} NotNode;
ASTNode *make_not_node(ASTNode *child);
int print_not_node(ASTNode *node, int indent, int offset, char *buffer,
                   size_t buffer_size);
void free_not_node(ASTNode *node);

/* ============ 列表操作 ============ */
// NODE_LIST
typedef enum {
  LIST_COLUMN,     // 列名列表
  LIST_VALUE,      // 值列表
  LIST_ASSIGNMENT, // 赋值列表
  LIST_COLUMN_DEF, // 列定义列表
  LIST_ORDER,      // orderby
  LIST_UNKNOWN
} ListType;

static inline const char *list_type_to_string(ListType type) {
  switch (type) {
  case LIST_COLUMN:
    return "COLUMNS";
  case LIST_VALUE:
    return "VALUES";
  case LIST_ASSIGNMENT:
    return "ASSIGNMENTS";
  case LIST_COLUMN_DEF:
    return "COLUMN_DEFINES";
  case LIST_ORDER:
    return "ORDER_BY";
  default:
    return "UNKNOWN_LIST_TYPE";
  }
}
typedef struct ASTNodeList {
  ListType list_type; // 列表类型 (列名列表、值列表、赋值列表等)
  ASTNode *head;      // 链表头指针
  ASTNode *tail;      // 链表尾指针
  int count;          // 链表节点数量
} ASTNodeList;

ASTNode *create_list(ASTNode *elem, ListType lst);
ASTNode *append_to_list(ASTNode *list_node, ASTNode *elem);
int print_list(ASTNode *node, int indent, int offset, char *buffer,
               size_t buffer_size);
void free_list(ASTNode *list_node);

// NODE_ORDER
typedef struct OrderNode {
  char *column;      // 列名
  COpType direction; // 排序方向 (OP_ASC 或 OP_DESC)
} OrderNode;
ASTNode *make_order_node(const char *column, COpType direction);
int print_order_node(ASTNode *node, int indent, int offset, char *buffer,
                     size_t buffer_size);
void free_order_node(ASTNode *node);

// NODE_LIMIT
typedef struct LimitNode {
  int limit;  // 限制数量
  int offset; // 偏移量
} LimitNode;

ASTNode *make_limit_node(int limit, int offset);
int print_limit_node(ASTNode *node, int indent, int offset, char *buffer,
                     size_t buffer_size);
void free_limit_node(ASTNode *node);

/* ============ DDL 节点 ============ */
// NODE_USE
typedef struct DatabaseNode {
  char *db_name; // 数据库名
  SSpan db_span; // 库名位置
} DatabaseNode;  // for create, drop,use

ASTNode *make_use_node(const char *db_name);
int print_use_node(ASTNode *node, int indent, int offset, char *buffer,
                   size_t buffer_size);
void free_use_node(ASTNode *node);

// NODE_CREATE_DATABASE
ASTNode *make_create_database_node(const char *db_name);
int print_create_database_node(ASTNode *node, int indent, int offset,
                               char *buffer, size_t buffer_size);
void free_create_database_node(ASTNode *node);

// NODE_DROP_DATABASE
ASTNode *make_drop_database_node(const char *db_name);
int print_drop_database_node(ASTNode *node, int indent, int offset,
                             char *buffer, size_t buffer_size);
void free_drop_database_node(ASTNode *node);

// NODE_CREATE_TABLE
typedef struct CreateTableNode {
  char *table_name; // 表名
  SSpan table_span; // 表名位置
  ASTNode *columns; // 列定义列表 (ASTNodeList)
} CreateTableNode;
ASTNode *make_create_table_node(const char *table_name, ASTNode *columns);
int print_create_table_node(ASTNode *node, int indent, int offset, char *buffer,
                            size_t buffer_size);
void free_create_table_node(ASTNode *node);

// NODE_DROP_TABLE
typedef struct DropTableNode {
  char *table_name; // 表名
  SSpan table_span; // 表名位置
} DropTableNode;
ASTNode *make_drop_table_node(const char *table_name);
int print_drop_table_node(ASTNode *node, int indent, int offset, char *buffer,
                          size_t buffer_size);
void free_drop_table_node(ASTNode *node);

// NODE_COLUMN_DEF
typedef struct ColumnDefNode {
  char *name;          // 列名
  CDataType data_type; // 数据类型
  unsigned length;     // 字符串类型的声明长度（0 = 未声明）
  int is_primary_key;  // 是否为主键
  int nullable;        // 是否允许为空
} ColumnDefNode;

ASTNode *make_column_def_node(const char *name, CDataType data_type,
                              unsigned length, int is_primary_key,
                              int nullable);

int print_column_def_node(ASTNode *node, int indent, int offset, char *buffer,
                          size_t buffer_size);
void free_column_def_node(ASTNode *node);

// AST 节点基类
typedef struct ASTNode {
  NodeType type;        // 标识当前节点到底是什么类型
  struct ASTNode *next; // 用于链表结构，指向下一个节点
  SSpan span;           // 该节点在 SQL 文本中的位置（未知时为全 0）
  __attribute__((aligned(16))) char data[];
} ASTNode;

// 记录节点位置（由 sql.y 的语法动作调用；位置来自 Bison 的 @$）
void ast_set_span(ASTNode *node, uint32_t begin_line, uint32_t begin_column,
                  uint32_t end_line, uint32_t end_column);

// 在语法动作里用：AST_SET_SPAN($$, @$);
#define AST_SET_SPAN(node, loc)                                                \
  do {                                                                         \
    if ((node) != NULL) {                                                      \
      ast_set_span((node), (uint32_t)(loc).first_line,                         \
                   (uint32_t)(loc).first_column, (uint32_t)(loc).last_line,    \
                   (uint32_t)(loc).last_column);                               \
    }                                                                          \
  } while (0)

#define AST_NODE_SIZE(data_size) (offsetof(ASTNode, data) + (data_size))

#define AST_NODE_DATA(node, type) ((type *)((node)->data))

#define USE_DATA(var, node, type) type *var = AST_NODE_DATA(node, type)

// 核心宏：通过 data 指针反推 ASTNode 基地址
// 注意：这里强转成 char* 是为了按字节进行指针算术运算
#define AST_NODE_FROM_DATA(data_ptr)                                           \
  ((ASTNode *)((char *)(data_ptr) - offsetof(ASTNode, data)))

// 获取 type 的便捷宏
#define GET_AST_TYPE(data_ptr) (AST_NODE_FROM_DATA(data_ptr)->type)

/* ============ 基础节点创建 ============ */
ASTNode *ast_alloc_node(NodeType type, size_t data_size);

/* ============ 全局变量 ============ */
extern ASTNode *g_parsed_ast;
/* ============ 管理函数 ============ */
void set_parsed_ast(ASTNode *node);
void reset_parser();
void print_ast(ASTNode *node, int indent);
void free_ast(ASTNode *node);
inline ASTNode *get_parsed_ast() { return g_parsed_ast; }

#ifdef __cplusplus
}
#endif

#endif // AST_H
