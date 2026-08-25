// ast.cpp
#include "ast.h"

/* 全局解析结果 */
ASTNode* g_parsed_ast = NULL;

/* ============ 基础节点创建 ============ */
ASTNode* create_node(NodeType type) {
  ASTNode* node = (ASTNode*)malloc(sizeof(ASTNode));
  memset(node, 0, sizeof(ASTNode));
  node->type = type;
  return node;
}

ASTNode* make_ident_node(const char* name) {
  ASTNode* node = create_node(NODE_IDENT);
  node->str_val = strdup(name);
  return node;
}

ASTNode* make_number_node(int num) {
  ASTNode* node = create_node(NODE_NUMBER);
  node->num_val = num;
  return node;
}

ASTNode* make_string_node(const char* str) {
  ASTNode* node = create_node(NODE_STRING);
  // 去掉可能的引号
  char* clean_str = strdup(str);
  size_t len = strlen(clean_str);
  if (len >= 2 && clean_str[0] == '\'' && clean_str[len - 1] == '\'') {
    clean_str[len - 1] = '\0';
    memmove(clean_str, clean_str + 1, len);
  }
  node->str_val = clean_str;
  return node;
}

/* ============ 列表操作 ============ */
ASTNodeList* create_list(ASTNode* node) {
  ASTNodeList* list = (ASTNodeList*)malloc(sizeof(ASTNodeList));
  list->node = node;
  list->next = NULL;
  return list;
}

ASTNodeList* append_to_list(ASTNodeList* list, ASTNode* node) {
  if (!list) return create_list(node);

  ASTNodeList* current = list;
  while (current->next) {
    current = current->next;
  }
  current->next = (ASTNodeList*)malloc(sizeof(ASTNodeList));
  current->next->node = node;
  current->next->next = NULL;
  return list;
}

void free_list(ASTNodeList* list) {
  if (!list) return;
  ASTNodeList* current = list;
  while (current) {
    ASTNodeList* next = current->next;
    if (current->node) free_ast(current->node);
    free(current);
    current = next;
  }
}

/* ============ SQL 语句节点 ============ */
ASTNode* make_use_node(const char* db_name) {
  ASTNode* node = create_node(NODE_USE);
  node->str_val = strdup(db_name);
  return node;
}

ASTNode* make_select_node(const char* table, ASTNodeList* columns, ASTNode* condition) {
  ASTNode* node = create_node(NODE_SELECT);
  node->str_val = strdup(table);      // FROM 的表
  if(columns){
    ASTNode* col_wrapper = create_node(NODE_LIST);
    col_wrapper->list = columns;
    node->left = col_wrapper;
  }

  node->right = condition;  // WHERE 条件（可能为 NULL）
  return node;
}

ASTNode* make_insert_node(const char* table, ASTNodeList* columns,
                          ASTNodeList* values) {
  ASTNode* node = create_node(NODE_INSERT);
  node->str_val = strdup(table);

  // 创建列包装节点
  ASTNode* col_wrapper = create_node(NODE_LIST);
  col_wrapper->list = columns;
  node->left = col_wrapper;

  // 创建值包装节点
  ASTNode* val_wrapper = create_node(NODE_LIST);
  val_wrapper->list = values;
  node->right = val_wrapper;

  return node;
}

ASTNode* make_update_node(const char* table, ASTNodeList* assignments,
                          ASTNode* condition) {
  ASTNode* node = create_node(NODE_UPDATE);
  node->str_val = strdup(table);

  // 创建列包装节点
  ASTNode* col_wrapper = create_node(NODE_LIST);
  col_wrapper->list = assignments;
  node->left = col_wrapper;
  node->right = condition;   // WHERE 条件（可能为 NULL）

  return node;
}

ASTNode* make_delete_node(const char* table, ASTNode* condition) {
  ASTNode* node = create_node(NODE_DELETE);
  node->str_val = strdup(table);
  node->right = condition;  // WHERE 条件（可能为 NULL）
  return node;
}

/* ============ 表达式节点 ============ */
ASTNode* make_compare_node(ASTNode* left, const char* op, ASTNode* right) {
  ASTNode* node = create_node(NODE_COMPARE);
  node->left = left;
  node->right = right;
  node->op = strdup(op);
  return node;
}

ASTNode* make_binary_node(ASTNode* left, const char* op, ASTNode* right) {
  ASTNode* node = create_node(NODE_BINARY_OP);
  node->left = left;
  node->right = right;
  node->op = strdup(op);
  return node;
}

ASTNode* make_assignment_node(ASTNode* left, ASTNode* value) {
  ASTNode* node = create_node(NODE_ASSIGNMENT);
  node->left = left;
  node->right = value;
  return node;
}

/* ============ 管理函数 ============ */
void set_parsed_ast(ASTNode* node) { g_parsed_ast = node; }

void print_ast(ASTNode* node, int indent) {
  if (!node) {
    printf("%*sNULL\n", indent, "");
    return;
  }

  const char* type_names[] = {"USE",     "SELECT",    "INSERT",     "UPDATE",
                              "DELETE",  "IDENT",     "NUMBER",     "STRING",
                              "COMPARE", "BINARY_OP", "ASSIGNMENT", "LIST"};

  // 打印缩进和类型
  printf("%*s%s", indent, "", type_names[node->type]);

  // 打印值
  if (node->str_val) {
    if (node->type == NODE_STRING) {
      printf(" '%s'", node->str_val);
    } else {
      printf(" '%s'", node->str_val);
    }
  }
  if (node->num_val) {
    printf(" %d", node->num_val);
  }
  if (node->op) {
    printf(" [%s]", node->op);
  }
  printf("\n");

  // 打印子节点
  if (node->left) {
    printf("%*sleft:\n", indent + 2, "");
    print_ast(node->left, indent + 4);
  }
  if (node->right) {
    printf("%*sright:\n", indent + 2, "");
    print_ast(node->right, indent + 4);
  }
  if (node->list) {
    printf("%*slist:\n", indent + 2, "");
    ASTNodeList* curr = node->list;
    while (curr) {
      print_ast(curr->node, indent + 4);
      curr = curr->next;
    }
  }
}

void free_ast(ASTNode* node) {
  if (!node) return;

  free_ast(node->left);
  free_ast(node->right);
  if (node->str_val) free(node->str_val);
  if (node->op) free(node->op);
  if (node->list) free_list(node->list);

  free(node);
}

void reset_parser(){
  free_ast(g_parsed_ast);
  g_parsed_ast = NULL;
}