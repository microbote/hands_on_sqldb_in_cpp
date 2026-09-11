// ast.cpp
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wcast-function-type"
#pragma GCC diagnostic ignored "-Wincompatible-pointer-types"

#include "ast.h"
#include <cassert>
#include <stdarg.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

size_t ast_min_buf_size = 4096;
size_t ast_max_buf_size = 4*4096;
#define MIN_BUF_SIZE ast_min_buf_size
#define MAX_BUF_SIZE ast_max_buf_size

int ast_indent_step = 1;
#define STEP ast_indent_step

int ast_debug = 0;

#define DEBUG_PRINT ast_debug


static void DEBUG(const char * fmt, ...) {
  if(DEBUG_PRINT) {
    fprintf(stderr, "[DEBUG] ");
    va_list args;
    va_start(args, fmt);
    vfprintf(stderr, fmt, args);
    va_end(args);
    fprintf(stderr, "\n");
  }
  
}
static int step(int indent) { return indent + STEP; }

/* 全局解析结果 */
ASTNode *g_parsed_ast = NULL;

/* ============================================================
   基础分配函数
   ============================================================ */
ASTNode *ast_alloc_node(NodeType type, size_t data_size) {
  size_t total_size = AST_NODE_SIZE(data_size);
  ASTNode *node = (ASTNode *)calloc(1, total_size);
  if (node == nullptr) {
    return NULL;
  }
  node->type = type;
  node->next = NULL;
  return node;
}

/* ============================================================
   辅助：安全追加字符串到 buffer
   ============================================================ */
static int safe_append(char *buf, size_t size, int offset, const char *fmt,
                       ...) {
  if (offset < 0) {
    return offset;
  }
  int remaining = (int)size - offset;
  if (remaining <= 0) {
    return offset;
  }
  va_list args;
  va_start(args, fmt);
  int written = vsnprintf(buf + offset, remaining, fmt, args);
  va_end(args);
  if (written < 0) {
    return offset;
  }
  if (written >= remaining) {
    // 截断，填满 buffer
    buf[size - 1] = '\0';
    return (int)size - 1;
  }
  return offset + written;
}

static int append_indent(char *buf, size_t size, int offset, int indent) {
  for (int i = 0; i < indent; i++) {
    offset = safe_append(buf, size, offset, "  ");
  }
  return offset;
}

static int print_node(ASTNode *node, int indent, int offset, char *buffer,
                      size_t buffer_size) {
  if (node == nullptr) {
    offset = append_indent(buffer, buffer_size, offset, indent);
    offset = safe_append(buffer, buffer_size, offset, "NULL\n");
    return offset;
  }

  NodeLifetime *ltm = get_node_lifetime(node->type);
  if ((ltm != nullptr) && (ltm->to_string != nullptr)) {
    return ltm->to_string(node, indent, offset, buffer, buffer_size);
  }

  offset = append_indent(buffer, buffer_size, offset, indent);
  offset = safe_append(buffer, buffer_size, offset, "UNKNOWN_NODE(%d)\n",
                       node->type);

  return offset;
}

static int print_title(const char *title, int indent, int offset, char *buffer,
                       size_t buffer_size) {
  offset = append_indent(buffer, buffer_size, offset, indent);
  offset = safe_append(buffer, buffer_size, offset, "%s\n", title);
  return offset;
}

/* ============================================================
   NODE_NUMBER
   ============================================================ */
ASTNode *make_number_node(int64_t num) {
  ASTNode *node = ast_alloc_node(NODE_NUMBER, sizeof(NumberNode));
  USE_DATA(data, node, NumberNode);
  if (data == nullptr) {
    return nullptr;
  }
  data->value = num;
  return node;
}

int print_number_node(ASTNode *node, int indent, int offset, char *buffer,
                      size_t buffer_size) {
  NumberNode *data = (NumberNode *)node->data;
  offset = append_indent(buffer, buffer_size, offset, indent);
  offset = safe_append(buffer, buffer_size, offset, "NUMBER(%lld)\n",
                       (long long)data->value);
  return offset;
}

void free_number_node(ASTNode *node) { free(node); }

/* ============================================================
   NODE_LITERAL (NULL / TRUE / FALSE)
   ============================================================ */
const char *literal_kind_to_string(LiteralKind kind) {
  switch (kind) {
  case LITERAL_NULL:
    return "NULL";
  case LITERAL_TRUE:
    return "TRUE";
  case LITERAL_FALSE:
    return "FALSE";
  default:
    return "UNKNOWN_LITERAL";
  }
}

ASTNode *make_literal_node(LiteralKind kind) {
  ASTNode *node = ast_alloc_node(NODE_LITERAL, sizeof(LiteralNode));
  USE_DATA(data, node, LiteralNode);
  if (data == nullptr) {
    return NULL;
  }
  data->kind = kind;
  return node;
}

int print_literal_node(ASTNode *node, int indent, int offset, char *buffer,
                       size_t buffer_size) {
  LiteralNode *data = (LiteralNode *)node->data;
  offset = append_indent(buffer, buffer_size, offset, indent);
  offset = safe_append(buffer, buffer_size, offset, "LITERAL(%s)\n",
                       literal_kind_to_string(data->kind));
  return offset;
}

void free_literal_node(ASTNode *node) { free(node); }

/* ============================================================
   NODE_STRING
   ============================================================ */
ASTNode *make_string_node(const char *str) {
  ASTNode *node = ast_alloc_node(NODE_STRING, sizeof(StringNode));
  USE_DATA(data, node, StringNode);
  if (data == nullptr) {
    return NULL;
  }
  data->value = strdup(str);
  return node;
}

int print_string_node(ASTNode *node, int indent, int offset, char *buffer,
                      size_t buffer_size) {
  StringNode *data = (StringNode *)node->data;
  offset = append_indent(buffer, buffer_size, offset, indent);
  offset = safe_append(buffer, buffer_size, offset, "STRING('%s')\n",
                       (data->value != nullptr) ? data->value : "");
  return offset;
}

void free_string_node(ASTNode *node) {
  USE_DATA(data, node, StringNode);
  if (data->value != nullptr) {
    free(data->value);
  }
  free(node);
}

/* ============================================================
   NODE_IDENT
   ============================================================ */
ASTNode *make_ident_node(const char *name) {
  ASTNode *node = ast_alloc_node(NODE_IDENT, sizeof(IdentNode));
  USE_DATA(data, node, IdentNode);
  if (data == nullptr) {
    return NULL;
  }
  data->name = strdup(name);
  return node;
}

int print_ident_node(ASTNode *node, int indent, int offset, char *buffer,
                     size_t buffer_size) {
  IdentNode *data = (IdentNode *)node->data;
  offset = append_indent(buffer, buffer_size, offset, indent);
  offset = safe_append(buffer, buffer_size, offset, "IDENTITY('%s')\n",
                       (data->name != nullptr) ? data->name : "");
  return offset;
}

void free_ident_node(ASTNode *node) {
  USE_DATA(data, node, IdentNode);
  if (data->name != nullptr) {
    free(data->name);
  }
  free(node);
}

/* ============================================================
   NODE_SELECT
   ============================================================ */
ASTNode *make_select_node(const char *table, ASTNode *columns,
                          ASTNode *condition, ASTNode *order_by,
                          ASTNode *limit) {
  ASTNode *node = ast_alloc_node(NODE_SELECT, sizeof(SelectNode));
  USE_DATA(data, node, SelectNode);
  if (data == nullptr) {
    return NULL;
  }
  data->table = strdup(table);
  data->columns = columns;
  data->condition = condition;
  data->order_by = order_by;
  data->limit = limit;
  return node;
}

int print_select_node(ASTNode *node, int indent, int offset, char *buffer,
                      size_t buffer_size) {
  SelectNode *data = (SelectNode *)node->data;
  offset = append_indent(buffer, buffer_size, offset, indent);
  offset = safe_append(buffer, buffer_size, offset, "SELECT(table='%s')\n",
                       (data->table != nullptr) ? data->table : "");
  // columns
  if (data->columns != nullptr) {
    // offset = print_title("COLUMNS:", step(indent), offset, buffer,
    // buffer_size);
    offset =
        print_node(data->columns, step(indent), offset, buffer, buffer_size);
  }
  // condition
  if (data->condition != nullptr) {
    offset = print_title("WHERE:", step(indent), offset, buffer, buffer_size);
    offset =
        print_node(data->condition, step(step(indent)), offset, buffer, buffer_size);
  }
  // order_by
  if (data->order_by != nullptr) {
    //offset = print_title("ORDER_BY:", step(indent), offset, buffer, buffer_size);
    offset =
        print_node(data->order_by, step(indent), offset, buffer, buffer_size);
  }
  // limit
  if (data->limit != nullptr) {
    // offset = print_title("LIMIT:", step(indent), offset, buffer,
    // buffer_size);
    offset = print_node(data->limit, step(indent), offset, buffer, buffer_size);
  }
  return offset;
}

void free_select_node(ASTNode *node) {
  USE_DATA(data, node, SelectNode);
  if (data->table != nullptr) {
    free(data->table);
  }
  if (data->columns != nullptr) {
    free_ast(data->columns);
  }
  if (data->condition != nullptr) {
    free_ast(data->condition);
  }
  if (data->order_by != nullptr) {
    free_ast(data->order_by);
  }
  if (data->limit != nullptr) {
    free_ast(data->limit);
  }
  free(node);
}

/* ============================================================
   NODE_INSERT
   ============================================================ */
ASTNode *make_insert_node(const char *table, ASTNode *columns,
                          ASTNode *values) {
  ASTNode *node = ast_alloc_node(NODE_INSERT, sizeof(InsertNode));
  USE_DATA(data, node, InsertNode);
  if (data == nullptr) {
    free(node);
    return NULL;
  }
  data->table = strdup(table);
  data->columns = columns;
  data->values = values;
  return node;
}

int print_insert_node(ASTNode *node, int indent, int offset, char *buffer,
                      size_t buffer_size) {
  InsertNode *data = (InsertNode *)node->data;
  offset = append_indent(buffer, buffer_size, offset, indent);
  offset = safe_append(buffer, buffer_size, offset, "INSERT(table='%s')\n",
                       (data->table != nullptr) ? data->table : "");
  if (data->columns != nullptr) {
    offset =
        print_node(data->columns, step(indent), offset, buffer, buffer_size);
  }else{
    offset = print_title("COLUMNS: NULL", step(indent), offset, buffer, buffer_size);
  }
  if (data->values != nullptr) {
    offset =
        print_node(data->values, step(indent), offset, buffer, buffer_size);
  }
  return offset;
}

void free_insert_node(ASTNode *node) {
  USE_DATA(data, node, InsertNode);
  if (data->table != nullptr) {
    free(data->table);
  }
  if (data->columns != nullptr) {
    free_ast(data->columns);
  }
  if (data->values != nullptr) {
    free_ast(data->values);
  }
  free(node);
}

/* ============================================================
   NODE_UPDATE
   ============================================================ */
ASTNode *make_update_node(const char *table, ASTNode *assignments,
                          ASTNode *condition) {
  ASTNode *node = ast_alloc_node(NODE_UPDATE, sizeof(UpdateNode));
  USE_DATA(data, node, UpdateNode);
  if (data == nullptr) {
    free(node);
    return NULL;
  }
  data->table = strdup(table);
  data->assignments = assignments;
  data->condition = condition;
  return node;
}

int print_update_node(ASTNode *node, int indent, int offset, char *buffer,
                      size_t buffer_size) {
  UpdateNode *data = (UpdateNode *)node->data;
  offset = append_indent(buffer, buffer_size, offset, indent);
  offset = safe_append(buffer, buffer_size, offset, "UPDATE(table='%s')\n",
                       (data->table != nullptr) ? data->table : "");
  if (data->assignments != nullptr) {
    offset = print_node(data->assignments, step(indent), offset, buffer,
                        buffer_size);
  }
  if (data->condition != nullptr) {
    offset = print_title("WHERE: ", step(indent), offset, buffer, buffer_size);
    offset =
        print_node(data->condition, step(step(indent)), offset, buffer, buffer_size);
  }
  return offset;
}

void free_update_node(ASTNode *node) {
  USE_DATA(data, node, UpdateNode);
  if (data->table != nullptr) {
    free(data->table);
  }
  if (data->assignments != nullptr) {
    free_ast(data->assignments);
  }
  if (data->condition != nullptr) {
    free_ast(data->condition);
  }
  free(node);
}

/* ============================================================
   NODE_DELETE
   ============================================================ */
ASTNode *make_delete_node(const char *table, ASTNode *condition) {
  ASTNode *node = ast_alloc_node(NODE_DELETE, sizeof(DeleteNode));
  USE_DATA(data, node, DeleteNode);
  if (data == nullptr) {
    free(node);
    return NULL;
  }
  data->table = strdup(table);
  data->condition = condition;
  return node;
}

int print_delete_node(ASTNode *node, int indent, int offset, char *buffer,
                      size_t buffer_size) {
  DeleteNode *data = (DeleteNode *)node->data;
  offset = append_indent(buffer, buffer_size, offset, indent);
  offset = safe_append(buffer, buffer_size, offset, "DELETE(table='%s')\n",
                       (data->table != nullptr) ? data->table : "");
  if (data->condition != nullptr) {
    offset = print_title("WHERE: ", step(indent), offset, buffer, buffer_size);
    offset =
        print_node(data->condition, step(step(indent)), offset, buffer, buffer_size);
  }
  return offset;
}

void free_delete_node(ASTNode *node) {
  USE_DATA(data, node, DeleteNode);
  if (data->table != nullptr) {
    free(data->table);
  }
  if (data->condition != nullptr) {
    free_ast(data->condition);
  }
  free(node);
}

/* ============================================================
   NODE_ASSIGNMENT
   ============================================================ */
ASTNode *make_assignment_node(const char *column, ASTNode *value) {
  ASTNode *node = ast_alloc_node(NODE_ASSIGNMENT, sizeof(AssignmentNode));
  USE_DATA(data, node, AssignmentNode);
  if (data == nullptr) {
    free(node);
    return NULL;
  }
  data->column = strdup(column);
  data->value = value;
  return node;
}

int print_assignment_node(ASTNode *node, int indent, int offset, char *buffer,
                          size_t buffer_size) {
  AssignmentNode *data = (AssignmentNode *)node->data;
  offset = append_indent(buffer, buffer_size, offset, indent);
  offset = safe_append(buffer, buffer_size, offset, "ASSIGN(column='%s',value=",
                       (data->column != nullptr) ? data->column : "");
  if (data->value == nullptr) {
    offset = safe_append(buffer, buffer_size, offset, "NULL ");
  } else {
    offset = print_node(data->value, 0, offset, buffer, buffer_size);
  }
  offset = safe_append(buffer, buffer_size, offset-1, ")\n");

  return offset;
}

void free_assignment_node(ASTNode *node) {
  AssignmentNode *data = (AssignmentNode *)node->data;
  if (data->column != nullptr) {
    free(data->column);
  }
  if (data->value != nullptr) {
    free_ast(data->value);
  }
  free(node);
}

/* ============================================================
   NODE_COMPARE
   ============================================================ */
ASTNode *make_compare_node(const char *column, COpType optype, ASTNode *right) {
  ASTNode *node = ast_alloc_node(NODE_COMPARE, sizeof(CompareNode));
  USE_DATA(data, node, CompareNode);
  if (data == nullptr) {
    free(node);
    return NULL;
  }
  data->column = strdup(column);
  data->op = optype;
  data->right = right;
  return node;
}

int print_compare_node(ASTNode *node, int indent, int offset, char *buffer,
                       size_t buffer_size) {
  CompareNode *data = (CompareNode *)node->data;
  offset = append_indent(buffer, buffer_size, offset, indent);
  offset = safe_append(
      buffer, buffer_size, offset, "COMPARE(column='%s', op='%s', value=",
      (data->column != nullptr) ? data->column : "", op_to_string(data->op));
  if (data->right != nullptr) {
    offset = print_node(data->right, 0, offset, buffer, buffer_size);
  } else {
    offset = safe_append(buffer, buffer_size, offset, "NULL ");
  }
  offset = safe_append(buffer, buffer_size, offset-1, ")\n");
  return offset;
}

void free_compare_node(ASTNode *node) {
  CompareNode *data = (CompareNode *)node->data;
  if (data->column != nullptr) {
    free(data->column);
  }
  if (data->right != nullptr) {
    free_ast(data->right);
  }
  free(node);
}

/* ============================================================
   NODE_IN / NODE_NOT_IN
   ============================================================ */
ASTNode *make_in_node(const char *column, ASTNode *values) {
  ASTNode *node = ast_alloc_node(NODE_IN, sizeof(InNode));
  USE_DATA(data, node, InNode);
  if (data == nullptr) {
    free(node);
    return NULL;
  }
  data->column = strdup(column);
  data->values = values;
  data->excluded = 0;
  return node;
}

ASTNode *make_not_in_node(const char *column, ASTNode *values) {
  ASTNode *node = make_in_node(column, values);
  USE_DATA(data, node, InNode);
  data->excluded = 1;
  return node;
}

int print_in_node(ASTNode *node, int indent, int offset, char *buffer,
                  size_t buffer_size) {
  InNode *data = (InNode *)node->data;
  offset = append_indent(buffer, buffer_size, offset, indent);
  offset = safe_append(buffer, buffer_size, offset, "%s(column='%s')\n",
                       data->excluded>0 ? "NOT_IN" : "IN",
                       (data->column != nullptr) ? data->column : "");
  if (data->values != nullptr) {
    offset =
        print_node(data->values, step(indent), offset, buffer, buffer_size);
  } else {
    offset = append_indent(buffer, buffer_size, offset, step(indent));
    offset = safe_append(buffer, buffer_size, offset, "VALUES: NULL\n");
  }

  return offset;
}

void free_in_node(ASTNode *node) {
  InNode *data = (InNode *)node->data;
  if (data->column != nullptr) {
    free(data->column);
  }
  if (data->values != nullptr) {
    free_ast(data->values);
  }
  free(node);
}

/* ============================================================
   NODE_BINARY_OP
   ============================================================ */
ASTNode *make_binary_node(ASTNode *left, COpType optype, ASTNode *right) {
  ASTNode *node = ast_alloc_node(NODE_BINARY_OP, sizeof(BinaryOpNode));
  USE_DATA(data, node, BinaryOpNode);
  if (data == nullptr) {
    free(node);
    return NULL;
  }
  data->left = left;
  data->op = optype;
  data->right = right;
  return node;
}

int print_binary_node(ASTNode *node, int indent, int offset, char *buffer,
                      size_t buffer_size) {
  BinaryOpNode *data = (BinaryOpNode *)node->data;
  offset = append_indent(buffer, buffer_size, offset, indent);
  offset = safe_append(buffer, buffer_size, offset, "BINARY(op=%s)\n",
                       op_to_string(data->op));
  if (data->left != nullptr) {
    offset = print_title("LEFT:", step(indent), offset, buffer, buffer_size);
    offset = print_node(data->left, step(step(indent)), offset, buffer, buffer_size);
  } else {
    offset =
        print_title("LEFT: NULL", step(indent), offset, buffer, buffer_size);
  }
  if (data->right != nullptr) {
    offset = print_title("RIGHT:", step(indent), offset, buffer, buffer_size);
    offset = print_node(data->right, step(step(indent)), offset, buffer, buffer_size);
  } else {
    offset =
        print_title("RIGHT: NULL", step(indent), offset, buffer, buffer_size);
  }
  return offset;
}

void free_binary_node(ASTNode *node) {
  BinaryOpNode *data = (BinaryOpNode *)node->data;
  if (data->left != nullptr) {
    free_ast(data->left);
  }
  if (data->right != nullptr) {
    free_ast(data->right);
  }
  free(node);
}

/* ============================================================
   NODE_NOT
   ============================================================ */
ASTNode *make_not_node(ASTNode *child) {
  ASTNode *node = ast_alloc_node(NODE_NOT, sizeof(NotNode));
  USE_DATA(data, node, NotNode);
  if (data == nullptr) {
    free(node);
    return NULL;
  }
  data->child = child;
  return node;
}

int print_not_node(ASTNode *node, int indent, int offset, char *buffer,
                   size_t buffer_size) {
  NotNode *data = (NotNode *)node->data;
  offset = append_indent(buffer, buffer_size, offset, indent);
  offset = safe_append(buffer, buffer_size, offset, "NOT\n");
  if (data->child != nullptr) {
    offset = print_title("CHILD:", step(indent), offset, buffer, buffer_size);
    offset = print_node(data->child, step(indent), offset, buffer, buffer_size);
  } else {
    offset =
        print_title("CHILD: NULL", step(indent), offset, buffer, buffer_size);
  }
  return offset;
}

void free_not_node(ASTNode *node) {
  NotNode *data = (NotNode *)node->data;
  if (data->child != nullptr) {
    free_ast(data->child);
  }
  free(node);
}

/* ============================================================
   NODE_LIST (ASTNodeList 操作)
   ============================================================ */
ASTNode *create_list(ASTNode *elem, ListType lst) {
  ASTNode *node = ast_alloc_node(NODE_LIST, sizeof(ASTNodeList));
  USE_DATA(list, node, ASTNodeList);
  if (list == nullptr) {
    return NULL;
  }
  list->list_type = lst;
  list->head = elem;
  list->tail = elem;
  list->count = 1;
  return node;
}

ASTNode *append_to_list(ASTNode *list_node, ASTNode *elem) {
  if (list_node == nullptr) {
    return create_list(elem, LIST_UNKNOWN);
  }
  if (elem == nullptr) {
    return list_node;
  }
  USE_DATA(list, list_node, ASTNodeList);
  list->tail->next = elem;
  list->tail = elem;
  list->count++;
  return list_node;
}

int print_list(ASTNode *node, int indent, int offset, char *buffer,
               size_t buffer_size) {
  ASTNodeList *list = (ASTNodeList *)node->data;
  offset = append_indent(buffer, buffer_size, offset, indent);
  offset = safe_append(buffer, buffer_size, offset, "%s(count=%d)\n",
                       list_type_to_string(list->list_type), list->count);
  ASTNode *curr = list->head;
  while (curr != nullptr) {
    offset = print_node(curr, step(indent), offset, buffer, buffer_size);
    curr = curr->next;
  }
  return offset;
}

  void free_list(ASTNode *list_node) {
    if (list_node == nullptr) {
      return;
    }
    USE_DATA(list, list_node, ASTNodeList);
    ASTNode *curr = list->head;
    while (curr != nullptr) {
      ASTNode *next = curr->next;
      free_ast(curr);
      curr = next;
    }
    free(list_node);
  }

/* ============================================================
   NODE_ORDER
   ============================================================ */
ASTNode *make_order_node(const char *column, COpType direction) {
  ASTNode *node = ast_alloc_node(NODE_ORDER, sizeof(OrderNode));
  USE_DATA(data, node, OrderNode);
  if (data == nullptr) {
    free(node);
    return NULL;
  }
  data->column = strdup(column);
  data->direction = direction;
  return node;
}

int print_order_node(ASTNode *node, int indent, int offset, char *buffer,
                     size_t buffer_size) {
  OrderNode *data = (OrderNode *)node->data;
  offset = append_indent(buffer, buffer_size, offset, indent);
  offset = safe_append(
      buffer, buffer_size, offset, "ORDER_BY(column='%s', dir=%s)\n",
      (data->column != nullptr) ? data->column : "", op_to_string(data->direction));
  return offset;
}

void free_order_node(ASTNode *node) {
  OrderNode *data = (OrderNode *)node->data;
  if (data->column != nullptr) {
    free(data->column);
  }
  free(node);
}

/* ============================================================
   NODE_LIMIT
   ============================================================ */
ASTNode *make_limit_node(int limit, int offset) {
  ASTNode *node = ast_alloc_node(NODE_LIMIT, sizeof(LimitNode));
  USE_DATA(data, node, LimitNode);
  if (data == nullptr) {
    free(node);
    return NULL;
  }
  data->limit = limit;
  data->offset = offset;
  return node;
}

int print_limit_node(ASTNode *node, int indent, int offset, char *buffer,
                     size_t buffer_size) {
  LimitNode *data = (LimitNode *)node->data;
  offset = append_indent(buffer, buffer_size, offset, indent);
  offset =
      safe_append(buffer, buffer_size, offset, "LIMIT(limit=%d, offset=%d)\n",
                  data->limit, data->offset);
  return offset;
}

void free_limit_node(ASTNode *node) { free(node); }

/* ============================================================
   NODE_USE
   ============================================================ */
ASTNode *make_use_node(const char *db_name) {
  ASTNode *node = ast_alloc_node(NODE_USE, sizeof(DatabaseNode));
  USE_DATA(data, node, DatabaseNode);
  if (data == nullptr) {
    return NULL;
  }
  data->db_name = strdup(db_name);
  return node;
}

int print_use_node(ASTNode *node, int indent, int offset, char *buffer,
                   size_t buffer_size) {
  DatabaseNode *data = (DatabaseNode *)node->data;
  offset = append_indent(buffer, buffer_size, offset, indent);
  offset = safe_append(buffer, buffer_size, offset, "USE_DATABASE('%s')\n",
                       (data->db_name != nullptr) ? data->db_name : "");
  return offset;
}

void free_use_node(ASTNode *node) {
  USE_DATA(data, node, DatabaseNode);
  if (data->db_name != nullptr) {
    free(data->db_name);
  }
  free(node);
}

/* ============================================================
   NODE_CREATE_DATABASE
   ============================================================ */
ASTNode *make_create_database_node(const char *db_name) {
  ASTNode *node = ast_alloc_node(NODE_CREATE_DATABASE, sizeof(DatabaseNode));
  USE_DATA(data, node, DatabaseNode);
  if (data == nullptr) {
    return NULL;
  }
  data->db_name = strdup(db_name);
  return node;
}

int print_create_database_node(ASTNode *node, int indent, int offset,
                               char *buffer, size_t buffer_size) {
  DatabaseNode *data = (DatabaseNode *)node->data;
  offset = append_indent(buffer, buffer_size, offset, indent);
  offset = safe_append(buffer, buffer_size, offset, "CREATE_DATABASE('%s')\n",
                       (data->db_name != nullptr) ? data->db_name : "");
  return offset;
}

void free_create_database_node(ASTNode *node) {
  USE_DATA(data, node, DatabaseNode);
  if (data->db_name != nullptr) {
    free(data->db_name);
  }
  free(node);
}

/* ============================================================
   NODE_DROP_DATABASE
   ============================================================ */
ASTNode *make_drop_database_node(const char *db_name) {
  ASTNode *node = ast_alloc_node(NODE_DROP_DATABASE, sizeof(DatabaseNode));
  USE_DATA(data, node, DatabaseNode);
  if (data == nullptr) {
    return NULL;
  }
  data->db_name = strdup(db_name);
  return node;
}

int print_drop_database_node(ASTNode *node, int indent, int offset,
                             char *buffer, size_t buffer_size) {
  DatabaseNode *data = (DatabaseNode *)node->data;
  offset = append_indent(buffer, buffer_size, offset, indent);
  offset = safe_append(buffer, buffer_size, offset, "DROP_DATABASE('%s')\n",
                       (data->db_name != nullptr) ? data->db_name : "");
  return offset;
}

void free_drop_database_node(ASTNode *node) {
  USE_DATA(data, node, DatabaseNode);
  if (data->db_name != nullptr) {
    free(data->db_name);
  }
  free(node);
}

/* ============================================================
   NODE_CREATE_TABLE
   ============================================================ */
ASTNode *make_create_table_node(const char *table_name, ASTNode *columns) {
  ASTNode *node = ast_alloc_node(NODE_CREATE_TABLE, sizeof(CreateTableNode));
  USE_DATA(data, node, CreateTableNode);
  if (data == nullptr) {
    free(node);
    return NULL;
  }
  data->table_name = strdup(table_name);
  data->columns = columns;
  return node;
}

int print_create_table_node(ASTNode *node, int indent, int offset, char *buffer,
                            size_t buffer_size) {
  CreateTableNode *data = (CreateTableNode *)node->data;
  offset = append_indent(buffer, buffer_size, offset, indent);
  offset =
      safe_append(buffer, buffer_size, offset, "CREATE_TABLE(table='%s')\n",
                  (data->table_name != nullptr) ? data->table_name : "");
  if (data->columns != nullptr) {
    offset = print_node(data->columns, step(indent), offset, buffer, buffer_size);
  }
  return offset;
}

void free_create_table_node(ASTNode *node) {
  CreateTableNode *data = (CreateTableNode *)node->data;
  if (data->table_name != nullptr) {
    free(data->table_name);
  }
  if (data->columns != nullptr) {
    free_ast(data->columns);
  }
  free(node);
}

/* ============================================================
   NODE_DROP_TABLE
   ============================================================ */
ASTNode *make_drop_table_node(const char *table_name) {
  ASTNode *node = ast_alloc_node(NODE_DROP_TABLE, sizeof(DropTableNode));
  USE_DATA(data, node, DropTableNode);
  if (data == nullptr) {
    free(node);
    return NULL;
  }
  data->table_name = strdup(table_name);
  return node;
}

int print_drop_table_node(ASTNode *node, int indent, int offset, char *buffer,
                          size_t buffer_size) {
  DropTableNode *data = (DropTableNode *)node->data;
  offset = append_indent(buffer, buffer_size, offset, indent);
  offset = safe_append(buffer, buffer_size, offset, "DROP_TABLE(table='%s')\n",
                       (data->table_name != nullptr) ? data->table_name : "");
  return offset;
}

void free_drop_table_node(ASTNode *node) {
  DropTableNode *data = (DropTableNode *)node->data;
  if (data->table_name != nullptr) {
    free(data->table_name);
  }
  free(node);
}

/* ============================================================
   NODE_COLUMN_DEF
   ============================================================ */
ASTNode *make_column_def_node(const char *name, CDataType data_type,
                              unsigned length, int is_primary_key,
                              int nullable) {
  ASTNode *node = ast_alloc_node(NODE_COLUMN_DEF, sizeof(ColumnDefNode));
  USE_DATA(data, node, ColumnDefNode);
  if (data == nullptr) {
    free(node);
    return NULL;
  }
  data->name = strdup(name);
  data->data_type = data_type;
  data->length = length;
  data->is_primary_key = is_primary_key;
  data->nullable = nullable;
  return node;
}

/* 类型名 + 可选长度，例如 VARCHAR(32) */
static void format_column_type(const ColumnDefNode *data, char *out,
                               size_t out_size) {
  if (data->length > 0 && (data->data_type == DT_CHAR ||
                           data->data_type == DT_VARCHAR)) {
    snprintf(out, out_size, "%s(%u)", data_type_to_string(data->data_type),
             data->length);
  } else {
    snprintf(out, out_size, "%s", data_type_to_string(data->data_type));
  }
}

int print_column_def_node(ASTNode *node, int indent, int offset, char *buffer,
                          size_t buffer_size) {
  ColumnDefNode *data = (ColumnDefNode *)node->data;
  char type_name[64];
  format_column_type(data, type_name, sizeof(type_name));
  offset = append_indent(buffer, buffer_size, offset, indent);
  offset = safe_append(buffer, buffer_size, offset,
                       "COLUMN_DEFINE(NAME='%s', DATA_TYPE=%s, PRIMARY_KEY=%d, "
                       "NULLABLE=%d)\n",
                       (data->name != nullptr) ? data->name : "",
                       type_name,
                       data->is_primary_key, data->nullable);
  return offset;
}

void free_column_def_node(ASTNode *node) {
  ColumnDefNode *data = (ColumnDefNode *)node->data;
  if (data->name != nullptr) {
    free(data->name);
  }
  free(node);
}

/* ============================================================
   NodeLifetime 注册表

   纯 C 风格：按 NodeType 的声明顺序逐个列出（不使用指定下标初始化），
   下面用 typedef 数组做一次编译期长度检查，避免加了节点却忘了登记。
   ============================================================ */
static NodeLifetime nodes[NODE_TYPE_COUNT] = {
    {NODE_NUMBER, (NodeDataConstructor)make_number_node,
     (NodeDataDestructor)free_number_node, (NodeDataPrinter)print_number_node},
    {NODE_STRING, (NodeDataConstructor)make_string_node,
     (NodeDataDestructor)free_string_node, (NodeDataPrinter)print_string_node},
    {NODE_IDENT, (NodeDataConstructor)make_ident_node,
     (NodeDataDestructor)free_ident_node, (NodeDataPrinter)print_ident_node},
    {NODE_LITERAL, (NodeDataConstructor)make_literal_node,
     (NodeDataDestructor)free_literal_node,
     (NodeDataPrinter)print_literal_node},
    {NODE_USE, (NodeDataConstructor)make_use_node,
     (NodeDataDestructor)free_use_node, (NodeDataPrinter)print_use_node},
    {NODE_SELECT, (NodeDataConstructor)make_select_node,
     (NodeDataDestructor)free_select_node, (NodeDataPrinter)print_select_node},
    {NODE_INSERT, (NodeDataConstructor)make_insert_node,
     (NodeDataDestructor)free_insert_node, (NodeDataPrinter)print_insert_node},
    {NODE_UPDATE, (NodeDataConstructor)make_update_node,
     (NodeDataDestructor)free_update_node, (NodeDataPrinter)print_update_node},
    {NODE_DELETE, (NodeDataConstructor)make_delete_node,
     (NodeDataDestructor)free_delete_node, (NodeDataPrinter)print_delete_node},
    {NODE_COMPARE, (NodeDataConstructor)make_compare_node,
     (NodeDataDestructor)free_compare_node,
     (NodeDataPrinter)print_compare_node},
    {NODE_BINARY_OP, (NodeDataConstructor)make_binary_node,
     (NodeDataDestructor)free_binary_node, (NodeDataPrinter)print_binary_node},
    {NODE_ASSIGNMENT, (NodeDataConstructor)make_assignment_node,
     (NodeDataDestructor)free_assignment_node,
     (NodeDataPrinter)print_assignment_node},
    {NODE_LIST, (NodeDataConstructor)create_list, (NodeDataDestructor)free_list,
     (NodeDataPrinter)print_list},
    {NODE_NOT, (NodeDataConstructor)make_not_node,
     (NodeDataDestructor)free_not_node, (NodeDataPrinter)print_not_node},
    {NODE_IN, (NodeDataConstructor)make_in_node,
     (NodeDataDestructor)free_in_node, (NodeDataPrinter)print_in_node},
    {NODE_NOT_IN, (NodeDataConstructor)make_not_in_node,
     (NodeDataDestructor)free_in_node, (NodeDataPrinter)print_in_node},
    {NODE_ORDER, (NodeDataConstructor)make_order_node,
     (NodeDataDestructor)free_order_node, (NodeDataPrinter)print_order_node},
    {NODE_LIMIT, (NodeDataConstructor)make_limit_node,
     (NodeDataDestructor)free_limit_node, (NodeDataPrinter)print_limit_node},
    {NODE_CREATE_DATABASE, (NodeDataConstructor)make_create_database_node,
     (NodeDataDestructor)free_create_database_node,
     (NodeDataPrinter)print_create_database_node},
    {NODE_DROP_DATABASE, (NodeDataConstructor)make_drop_database_node,
     (NodeDataDestructor)free_drop_database_node,
     (NodeDataPrinter)print_drop_database_node},
    {NODE_CREATE_TABLE, (NodeDataConstructor)make_create_table_node,
     (NodeDataDestructor)free_create_table_node,
     (NodeDataPrinter)print_create_table_node},
    {NODE_DROP_TABLE, (NodeDataConstructor)make_drop_table_node,
     (NodeDataDestructor)free_drop_table_node,
     (NodeDataPrinter)print_drop_table_node},
    {NODE_COLUMN_DEF, (NodeDataConstructor)make_column_def_node,
     (NodeDataDestructor)free_column_def_node,
     (NodeDataPrinter)print_column_def_node},
};

/* 编译期检查：登记表必须覆盖 NODE_TYPE_COUNT 个节点（纯 C，不用 C++） */
typedef char node_lifetime_table_size_check
    [(sizeof(nodes) / sizeof(nodes[0])) == NODE_TYPE_COUNT ? 1 : -1];

NodeLifetime *get_node_lifetime(NodeType type) {
  if (type < 0 || type >= NODE_TYPE_COUNT) {
    return NULL;
  }
  return &nodes[type];
}

/* ============================================================
   管理函数
   ============================================================ */
void set_parsed_ast(ASTNode *node) { g_parsed_ast = node; }

void reset_parser() {
  if (g_parsed_ast != nullptr) {
    free_ast(g_parsed_ast);
    g_parsed_ast = NULL;
  }
}

void free_ast(ASTNode *node) {
  if (node == nullptr) {
    return;
  }
  NodeLifetime *obj = get_node_lifetime(node->type);
  if ((obj != nullptr) && (obj->do_free != nullptr)) {
    DEBUG("free(NODE:%s), %p freed\n", node_type_to_string(node->type), node);
    obj->do_free(node);
    
  } else {
    // fallback: 只释放节点本身（不处理子节点）
    DEBUG("free_ast: node type %d, %p freed (no free function)\n", node->type,
           node);
    free(node);
  }
}

/* ============================================================
   print_ast：分配 buffer，调用根节点的打印函数
   ============================================================ */
void print_ast(ASTNode *node, int indent) {
  if (node == NULL) {
    printf("NULL\n");
    return;
  }
  NodeLifetime *ltm = get_node_lifetime(node->type);
  if ((ltm == nullptr) || (ltm->to_string == nullptr)) {
    printf("UNKNOWN NODE\n");
    return;
  }

  // 初始 buffer 大小 4096，循环扩容直到足够
  size_t buf_size = MIN_BUF_SIZE;
  char *buffer = (char *)malloc(buf_size);
  if (buffer == nullptr) {
    return;
  }

  int offset = 0;
  while (true) {
    offset = ltm->to_string(node, indent, 0, buffer, buf_size);
    if (offset < 0) {
      // 错误，直接返回
      free(buffer);
      return;
    }
    if (offset < (int)buf_size - 1) {
      // buffer 够用
      break;
    }
    if (buf_size >= MAX_BUF_SIZE) {
      // 超过最大限制，直接截断
      break;
    }
    // 不够，扩容
    buf_size *= 2;
    char *new_buf = (char *)realloc(buffer, buf_size);
    if (new_buf == nullptr) {
      free(buffer);
      return;
    }
    buffer = new_buf;
  }
  printf("%s\n", buffer);
  free(buffer);
}
