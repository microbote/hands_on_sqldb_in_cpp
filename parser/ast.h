// ast.h
#ifndef AST_H
#define AST_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ============ AST 节点类型枚举 ============ */
typedef enum {
  NODE_USE,
  NODE_SELECT,
  NODE_INSERT,
  NODE_UPDATE,
  NODE_DELETE,
  NODE_IDENT,
  NODE_NUMBER,
  NODE_STRING,
  NODE_COMPARE,
  NODE_BINARY_OP,
  NODE_ASSIGNMENT,
  NODE_LIST
} NodeType;

/* ============ AST 节点结构 ============ */
typedef struct ASTNode {
  NodeType type;
  char* str_val;  // 用于标识符、字符串等
  int num_val;    // 用于数字
  char* op;       // 运算符（">", "=", "AND", "OR" 等）
  struct ASTNode* left;
  struct ASTNode* right;
  struct ASTNodeList* list;

} ASTNode;

/* ============ AST 链表结构 ============ */
typedef struct ASTNodeList {
  ASTNode* node;
  struct ASTNodeList* next;
} ASTNodeList;

/* ============ 全局变量 ============ */
extern ASTNode* g_parsed_ast;

/* ============ 基础节点创建 ============ */
ASTNode* create_node(NodeType type);
ASTNode* make_ident_node(const char* name);
ASTNode* make_number_node(int num);
ASTNode* make_string_node(const char* str);

/* ============ 列表操作 ============ */
ASTNodeList* create_list(ASTNode* node);
ASTNodeList* append_to_list(ASTNodeList* list, ASTNode* node);
void free_list(ASTNodeList* list);

/* ============ SQL 语句节点 ============ */
ASTNode* make_use_node(const char* db_name);
ASTNode* make_select_node(const char* table, ASTNodeList * columns, ASTNode * condition);
ASTNode* make_insert_node(const char* table, ASTNodeList* columns,
                          ASTNodeList* values);
ASTNode* make_update_node(const char* table, ASTNodeList* assignments,
                          ASTNode* condition);
ASTNode* make_delete_node(const char* table, ASTNode* condition);

/* ============ 表达式节点 ============ */
ASTNode* make_compare_node(ASTNode* left, const char* op, ASTNode* right);
ASTNode* make_binary_node(ASTNode* left, const char* op, ASTNode* right);
ASTNode* make_assignment_node(ASTNode* column, ASTNode* value);

/* ============ 管理函数 ============ */
void set_parsed_ast(ASTNode* node);
void reset_parser();
void print_ast(ASTNode* node, int indent);
void free_ast(ASTNode* node);
inline ASTNode* get_parsed_ast() { return g_parsed_ast; }

#ifdef __cplusplus
}
#endif

#endif  // AST_H