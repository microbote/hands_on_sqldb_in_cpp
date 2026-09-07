// tests/test_ast.cpp
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <assert.h>
#include <string>

#include "parser/ast.h"

/* ============================================================
   辅助函数
   ============================================================ */
#define TEST_PASS() printf("  ✅ PASS\n")
#define TEST_FAIL(msg) printf("  ❌ FAIL: %s\n", msg)
#define TEST_ASSERT(expr, msg) \
    do { \
        if (!(expr)) { \
            printf("  ❌ FAIL: %s\n", msg); \
            return 1; \
        } \
    } while (0)

int g_test_count = 0;
int g_pass_count = 0;

void print_separator(const char* title) {
    printf("\n%s\n", title);
    printf("%s\n", std::string(strlen(title), '=').c_str());
}

#define RUN_TEST(name) \
    do { \
        g_test_count++; \
        printf("\n--- %s ---\n", #name); \
        if (test_##name() == 0) { \
            g_pass_count++; \
        } \
    } while (0)

/* ============================================================
   测试：基础节点创建
   ============================================================ */
int test_basic_nodes() {
    printf("测试基础节点创建...\n");
    
    // NODE_NUMBER
    ASTNode* num = make_number_node(42);
    TEST_ASSERT(num != NULL, "make_number_node failed");
    TEST_ASSERT(num->type == NODE_NUMBER, "NODE_NUMBER type mismatch");
    NumberNode* num_data = (NumberNode*)num->data;
    TEST_ASSERT(num_data->value == 42, "NumberNode value mismatch");
    
    // NODE_STRING
    ASTNode* str = make_string_node("hello");
    TEST_ASSERT(str != NULL, "make_string_node failed");
    TEST_ASSERT(str->type == NODE_STRING, "NODE_STRING type mismatch");
    StringNode* str_data = (StringNode*)str->data;
    TEST_ASSERT(strcmp(str_data->value, "hello") == 0, "StringNode value mismatch");
    
    // NODE_IDENT
    ASTNode* ident = make_ident_node("users");
    TEST_ASSERT(ident != NULL, "make_ident_node failed");
    TEST_ASSERT(ident->type == NODE_IDENT, "NODE_IDENT type mismatch");
    IdentNode* ident_data = (IdentNode*)ident->data;
    TEST_ASSERT(strcmp(ident_data->name, "users") == 0, "IdentNode name mismatch");
    
    // 打印测试
    char buffer[1024];
    int offset = 0;
    offset = print_number_node(num, 0, offset, buffer, sizeof(buffer));
    TEST_ASSERT(offset > 0, "print_number_node failed");
    
    offset = 0;
    offset = print_string_node(str, 0, offset, buffer, sizeof(buffer));
    TEST_ASSERT(offset > 0, "print_string_node failed");
    
    offset = 0;
    offset = print_ident_node(ident, 0, offset, buffer, sizeof(buffer));
    TEST_ASSERT(offset > 0, "print_ident_node failed");
    
    // 释放
    free_ast(num);
    free_ast(str);
    free_ast(ident);
    
    TEST_PASS();
    return 0;
}

/* ============================================================
   测试：列表操作
   ============================================================ */
int test_list_operations() {
    printf("测试列表操作...\n");
    
    // 创建列表
    ASTNode* item1 = make_ident_node("id");
    ASTNode* item2 = make_ident_node("name");
    ASTNode* item3 = make_ident_node("age");
    
    ASTNode* list = create_list(item1, LIST_COLUMN);
    TEST_ASSERT(list != NULL, "create_list failed");
    TEST_ASSERT(list->type == NODE_LIST, "NODE_LIST type mismatch");
    
    ASTNodeList* list_data = (ASTNodeList*)list->data;
    TEST_ASSERT(list_data->count == 1, "list count should be 1");
    TEST_ASSERT(list_data->head == item1, "list head mismatch");
    TEST_ASSERT(list_data->tail == item1, "list tail mismatch");
    
    // 追加
    list = append_to_list(list, item2);
    TEST_ASSERT(list_data->count == 2, "list count should be 2");
    TEST_ASSERT(list_data->head == item1, "list head mismatch");
    TEST_ASSERT(list_data->tail == item2, "list tail mismatch");
    TEST_ASSERT(item1->next == item2, "item1->next should be item2");
    
    list = append_to_list(list, item3);
    TEST_ASSERT(list_data->count == 3, "list count should be 3");
    TEST_ASSERT(list_data->tail == item3, "list tail mismatch");
    TEST_ASSERT(item2->next == item3, "item2->next should be item3");
    
    // 打印列表
    char buffer[1024];
    int offset = print_list(list, 0, 0, buffer, sizeof(buffer));
    TEST_ASSERT(offset > 0, "print_list failed");
    printf("List print: %s", buffer);
    
    // 释放
    free_ast(list);
    
    TEST_PASS();
    return 0;
}

/* ============================================================
   测试：SELECT 节点
   ============================================================ */
int test_select_node() {
    printf("测试 SELECT 节点...\n");
    
    // 构建 SELECT * FROM users
    ASTNode* columns = create_list(make_ident_node("*"), LIST_COLUMN);
    ASTNode* select = make_select_node("users", columns, NULL, NULL, NULL);
    TEST_ASSERT(select != NULL, "make_select_node failed");
    TEST_ASSERT(select->type == NODE_SELECT, "NODE_SELECT type mismatch");
    
    SelectNode* data = (SelectNode*)select->data;
    TEST_ASSERT(strcmp(data->table, "users") == 0, "table name mismatch");
    TEST_ASSERT(data->columns != NULL, "columns should not be NULL");
    TEST_ASSERT(data->condition == NULL, "condition should be NULL");
    TEST_ASSERT(data->order_by == NULL, "order_by should be NULL");
    TEST_ASSERT(data->limit == NULL, "limit should be NULL");
    
    // 带 WHERE 条件的 SELECT
    ASTNode* cond = make_compare_node("age", OP_GT, make_number_node(18));
    columns = create_list(make_ident_node("id"), LIST_COLUMN);
    columns = append_to_list(columns, make_ident_node("name"));
    select = make_select_node("users", columns, cond, NULL, NULL);
    TEST_ASSERT(select != NULL, "make_select_node with condition failed");
    
    SelectNode* data2 = (SelectNode*)select->data;
    TEST_ASSERT(data2->condition != NULL, "condition should not be NULL");
    TEST_ASSERT(data2->condition->type == NODE_COMPARE, "condition type mismatch");
    
    // 带 ORDER BY 的 SELECT
    ASTNode* order = make_order_node("id", OP_ASC);
    select = make_select_node("users", NULL, NULL, order, NULL);
    TEST_ASSERT(select != NULL, "make_select_node with order_by failed");
    SelectNode* data3 = (SelectNode*)select->data;
    TEST_ASSERT(data3->order_by != NULL, "order_by should not be NULL");
    
    // 带 LIMIT 的 SELECT
    ASTNode* limit = make_limit_node(10, 0);
    select = make_select_node("users", NULL, NULL, NULL, limit);
    TEST_ASSERT(select != NULL, "make_select_node with limit failed");
    SelectNode* data4 = (SelectNode*)select->data;
    TEST_ASSERT(data4->limit != NULL, "limit should not be NULL");
    LimitNode* limit_data = (LimitNode*)limit->data;
    TEST_ASSERT(limit_data->limit == 10, "limit value mismatch");
    TEST_ASSERT(limit_data->offset == 0, "offset value mismatch");
    
    // 打印
    char buffer[2048];
    int offset = print_select_node(select, 0, 0, buffer, sizeof(buffer));
    TEST_ASSERT(offset > 0, "print_select_node failed");
    printf("Select print:\n%s", buffer);
    
    free_ast(select);
    TEST_PASS();
    return 0;
}

/* ============================================================
   测试：INSERT 节点
   ============================================================ */
int test_insert_node() {
    printf("测试 INSERT 节点...\n");
    
    // INSERT INTO users (id, name) VALUES (1, 'Alice')
    ASTNode* columns = create_list(make_ident_node("id"), LIST_COLUMN);
    columns = append_to_list(columns, make_ident_node("name"));
    
    ASTNode* values = create_list(make_number_node(1), LIST_VALUE);
    values = append_to_list(values, make_string_node("Alice"));
    
    ASTNode* insert = make_insert_node("users", columns, values);
    TEST_ASSERT(insert != NULL, "make_insert_node failed");
    TEST_ASSERT(insert->type == NODE_INSERT, "NODE_INSERT type mismatch");
    
    InsertNode* data = (InsertNode*)insert->data;
    TEST_ASSERT(strcmp(data->table, "users") == 0, "table name mismatch");
    TEST_ASSERT(data->columns != NULL, "columns should not be NULL");
    TEST_ASSERT(data->values != NULL, "values should not be NULL");
    
    // 检查列列表
    ASTNodeList* col_list = (ASTNodeList*)data->columns->data;
    TEST_ASSERT(col_list->count == 2, "column count should be 2");
    
    // 检查值列表
    ASTNodeList* val_list = (ASTNodeList*)data->values->data;
    TEST_ASSERT(val_list->count == 2, "value count should be 2");
    
    // 打印
    char buffer[2048];
    int offset = print_insert_node(insert, 0, 0, buffer, sizeof(buffer));
    TEST_ASSERT(offset > 0, "print_insert_node failed");
    printf("Insert print:\n%s", buffer);
    
    free_ast(insert);
    TEST_PASS();
    return 0;
}

/* ============================================================
   测试：UPDATE 节点
   ============================================================ */
int test_update_node() {
    printf("测试 UPDATE 节点...\n");
    
    // UPDATE users SET name = 'Jane', age = 30 WHERE id = 1
    ASTNode* assignments = create_list(
        make_assignment_node("name", make_string_node("Jane")), LIST_ASSIGNMENT);
    assignments = append_to_list(assignments, 
        make_assignment_node("age", make_number_node(30)));
    
    ASTNode* cond = make_compare_node("id", OP_EQ, make_number_node(1));
    ASTNode* update = make_update_node("users", assignments, cond);
    
    TEST_ASSERT(update != NULL, "make_update_node failed");
    TEST_ASSERT(update->type == NODE_UPDATE, "NODE_UPDATE type mismatch");
    
    UpdateNode* data = (UpdateNode*)update->data;
    TEST_ASSERT(strcmp(data->table, "users") == 0, "table name mismatch");
    TEST_ASSERT(data->assignments != NULL, "assignments should not be NULL");
    TEST_ASSERT(data->condition != NULL, "condition should not be NULL");
    
    ASTNodeList* assign_list = (ASTNodeList*)data->assignments->data;
    TEST_ASSERT(assign_list->count == 2, "assignment count should be 2");
    
    // 打印
    char buffer[2048];
    int offset = print_update_node(update, 0, 0, buffer, sizeof(buffer));
    TEST_ASSERT(offset > 0, "print_update_node failed");
    printf("Update print:\n%s", buffer);
    
    free_ast(update);
    TEST_PASS();
    return 0;
}

/* ============================================================
   测试：DELETE 节点
   ============================================================ */
int test_delete_node() {
    printf("测试 DELETE 节点...\n");
    
    // DELETE FROM users WHERE id = 1
    ASTNode* cond = make_compare_node("id", OP_EQ, make_number_node(1));
    ASTNode* del = make_delete_node("users", cond);
    
    TEST_ASSERT(del != NULL, "make_delete_node failed");
    TEST_ASSERT(del->type == NODE_DELETE, "NODE_DELETE type mismatch");
    
    DeleteNode* data = (DeleteNode*)del->data;
    TEST_ASSERT(strcmp(data->table, "users") == 0, "table name mismatch");
    TEST_ASSERT(data->condition != NULL, "condition should not be NULL");
    
    // 打印
    char buffer[1024];
    int offset = print_delete_node(del, 0, 0, buffer, sizeof(buffer));
    TEST_ASSERT(offset > 0, "print_delete_node failed");
    printf("Delete print:\n%s", buffer);
    
    free_ast(del);
    TEST_PASS();
    return 0;
}

/* ============================================================
   测试：条件表达式节点
   ============================================================ */
int test_condition_nodes() {
    printf("测试条件表达式节点...\n");
    
    // COMPARE: age > 18
    ASTNode* compare = make_compare_node("age", OP_GT, make_number_node(18));
    TEST_ASSERT(compare != NULL, "make_compare_node failed");
    TEST_ASSERT(compare->type == NODE_COMPARE, "NODE_COMPARE type mismatch");
    CompareNode* cmp_data = (CompareNode*)compare->data;
    TEST_ASSERT(strcmp(cmp_data->column, "age") == 0, "column name mismatch");
    TEST_ASSERT(cmp_data->op == OP_GT, "op mismatch");
    TEST_ASSERT(cmp_data->right != NULL, "right should not be NULL");
    
    // IN: id IN (1, 2, 3)
    ASTNode* values = create_list(make_number_node(1), LIST_VALUE);
    values = append_to_list(values, make_number_node(2));
    values = append_to_list(values, make_number_node(3));
    ASTNode* in_node = make_in_node("id", values);
    TEST_ASSERT(in_node != NULL, "make_in_node failed");
    TEST_ASSERT(in_node->type == NODE_IN, "NODE_IN type mismatch");
    InNode* in_data = (InNode*)in_node->data;
    TEST_ASSERT(strcmp(in_data->column, "id") == 0, "column name mismatch");
    TEST_ASSERT(in_data->excluded == 0, "excluded should be 0");
    TEST_ASSERT(in_data->values != NULL, "values should not be NULL");
    
    // NOT IN
    ASTNode* not_in = make_not_in_node("id", values);
    TEST_ASSERT(not_in != NULL, "make_not_in_node failed");
    InNode* not_in_data = (InNode*)not_in->data;
    TEST_ASSERT(not_in_data->excluded == 1, "excluded should be 1");
    
    // BINARY_OP: age > 18 AND status = 'active'
    ASTNode* left = make_compare_node("age", OP_GT, make_number_node(18));
    ASTNode* right = make_compare_node("status", OP_EQ, make_string_node("active"));
    ASTNode* binary = make_binary_node(left, OP_AND, right);
    TEST_ASSERT(binary != NULL, "make_binary_node failed");
    TEST_ASSERT(binary->type == NODE_BINARY_OP, "NODE_BINARY_OP type mismatch");
    BinaryOpNode* bin_data = (BinaryOpNode*)binary->data;
    TEST_ASSERT(bin_data->op == OP_AND, "op mismatch");
    TEST_ASSERT(bin_data->left != NULL, "left should not be NULL");
    TEST_ASSERT(bin_data->right != NULL, "right should not be NULL");
    
    // NOT
    ASTNode* not_node = make_not_node(compare);
    TEST_ASSERT(not_node != NULL, "make_not_node failed");
    TEST_ASSERT(not_node->type == NODE_NOT, "NODE_NOT type mismatch");
    NotNode* not_data = (NotNode*)not_node->data;
    TEST_ASSERT(not_data->child != NULL, "child should not be NULL");
    
    // 打印
    char buffer[2048];
    int offset = print_binary_node(binary, 0, 0, buffer, sizeof(buffer));
    printf("Binary print:\n%s", buffer);
    
    free_ast(binary);
    free_ast(not_node);
    TEST_PASS();
    return 0;
}

/* ============================================================
   测试：DDL 节点
   ============================================================ */
int test_ddl_nodes() {
    printf("测试 DDL 节点...\n");
    
    // CREATE TABLE users (id INT PRIMARY KEY, name VARCHAR NOT NULL)
    ASTNode* columns = create_list(
        make_column_def_node("id", DT_INT, 1, 0), LIST_COLUMN_DEF);
    columns = append_to_list(columns, 
        make_column_def_node("name", DT_VARCHAR, 0, 0));
    columns = append_to_list(columns, 
        make_column_def_node("age", DT_INT, 0, 1));
    
    ASTNode* create = make_create_table_node("users", columns);
    TEST_ASSERT(create != NULL, "make_create_table_node failed");
    TEST_ASSERT(create->type == NODE_CREATE_TABLE, "NODE_CREATE_TABLE type mismatch");
    
    CreateTableNode* create_data = (CreateTableNode*)create->data;
    TEST_ASSERT(strcmp(create_data->table_name, "users") == 0, "table name mismatch");
    TEST_ASSERT(create_data->columns != NULL, "columns should not be NULL");
    
    // 检查列定义
    ASTNodeList* col_list = (ASTNodeList*)create_data->columns->data;
    TEST_ASSERT(col_list->count == 3, "column count should be 3");
    
    // DROP TABLE
    ASTNode* drop = make_drop_table_node("users");
    TEST_ASSERT(drop != NULL, "make_drop_table_node failed");
    TEST_ASSERT(drop->type == NODE_DROP_TABLE, "NODE_DROP_TABLE type mismatch");
    DropTableNode* drop_data = (DropTableNode*)drop->data;
    TEST_ASSERT(strcmp(drop_data->table_name, "users") == 0, "table name mismatch");
    
    // CREATE DATABASE
    ASTNode* create_db = make_create_database_node("testdb");
    TEST_ASSERT(create_db != NULL, "make_create_database_node failed");
    TEST_ASSERT(create_db->type == NODE_CREATE_DATABASE, "NODE_CREATE_DATABASE type mismatch");
    
    // DROP DATABASE
    ASTNode* drop_db = make_drop_database_node("testdb");
    TEST_ASSERT(drop_db != NULL, "make_drop_database_node failed");
    TEST_ASSERT(drop_db->type == NODE_DROP_DATABASE, "NODE_DROP_DATABASE type mismatch");
    
    // USE
    ASTNode* use = make_use_node("testdb");
    TEST_ASSERT(use != NULL, "make_use_node failed");
    TEST_ASSERT(use->type == NODE_USE, "NODE_USE type mismatch");
    DatabaseNode* use_data = (DatabaseNode*)use->data;
    TEST_ASSERT(strcmp(use_data->db_name, "testdb") == 0, "db name mismatch");
    
    // 打印
    char buffer[2048];
    int offset = print_create_table_node(create, 0, 0, buffer, sizeof(buffer));
    printf("Create table print:\n%s", buffer);
    
    free_ast(create);
    free_ast(drop);
    free_ast(create_db);
    free_ast(drop_db);
    free_ast(use);
    TEST_PASS();
    return 0;
}

/* ============================================================
   测试：ORDER BY 和 LIMIT 节点
   ============================================================ */
int test_order_limit_nodes() {
    printf("测试 ORDER BY 和 LIMIT 节点...\n");
    
    // ORDER BY
    ASTNode* order_asc = make_order_node("id", OP_ASC);
    TEST_ASSERT(order_asc != NULL, "make_order_node ASC failed");
    OrderNode* order_data = (OrderNode*)order_asc->data;
    TEST_ASSERT(strcmp(order_data->column, "id") == 0, "column name mismatch");
    TEST_ASSERT(order_data->direction == OP_ASC, "direction should be ASC");
    
    ASTNode* order_desc = make_order_node("name", OP_DESC);
    TEST_ASSERT(order_desc != NULL, "make_order_node DESC failed");
    OrderNode* order_desc_data = (OrderNode*)order_desc->data;
    TEST_ASSERT(order_desc_data->direction == OP_DESC, "direction should be DESC");
    
    // LIMIT
    ASTNode* limit = make_limit_node(10, 20);
    TEST_ASSERT(limit != NULL, "make_limit_node failed");
    LimitNode* limit_data = (LimitNode*)limit->data;
    TEST_ASSERT(limit_data->limit == 10, "limit value mismatch");
    TEST_ASSERT(limit_data->offset == 20, "offset value mismatch");
    
    // 打印
    char buffer[1024];
    int offset = print_limit_node(limit, 0, 0, buffer, sizeof(buffer));
    TEST_ASSERT(offset > 0, "print_limit_node failed");
    printf("Limit print: %s", buffer);
    
    free_ast(order_asc);
    free_ast(order_desc);
    free_ast(limit);
    TEST_PASS();
    return 0;
}

/* ============================================================
   测试：NodeLifetime 注册表
   ============================================================ */
int test_node_lifetime() {
    printf("测试 NodeLifetime 注册表...\n");
    
    // 检查所有节点类型是否都有生命周期记录
    for (int i = 0; i < NODE_TYPE_COUNT; i++) {
        NodeLifetime* lt = get_node_lifetime((NodeType)i);
        TEST_ASSERT(lt != NULL, "get_node_lifetime returned NULL");
        TEST_ASSERT(lt->type == i, "type mismatch");
        // 析构函数应该存在
        TEST_ASSERT(lt->do_free != NULL, "do_free is NULL");
    }
    
    // 检查超出范围
    NodeLifetime* lt = get_node_lifetime((NodeType)999);
    TEST_ASSERT(lt == NULL, "get_node_lifetime should return NULL for out of range");
    
    TEST_PASS();
    return 0;
}

/* ============================================================
   测试：复杂 AST 树
   ============================================================ */
int test_complex_ast() {
    printf("测试复杂 AST 树...\n");
    
    // 构建:
    // SELECT id, name FROM users 
    // WHERE age > 18 AND status = 'active' 
    // ORDER BY id DESC 
    // LIMIT 10 OFFSET 5
    
    // 列列表
    ASTNode* columns = create_list(make_ident_node("id"), LIST_COLUMN);
    columns = append_to_list(columns, make_ident_node("name"));
    
    // WHERE 条件: age > 18 AND status = 'active'
    ASTNode* cond1 = make_compare_node("age", OP_GT, make_number_node(18));
    ASTNode* cond2 = make_compare_node("status", OP_EQ, make_string_node("active"));
    ASTNode* condition = make_binary_node(cond1, OP_AND, cond2);
    
    // ORDER BY: id DESC
    ASTNode* order = make_order_node("id", OP_DESC);
    
    // LIMIT: 10 OFFSET 5
    ASTNode* limit = make_limit_node(10, 5);
    
    // SELECT
    ASTNode* select = make_select_node("users", columns, condition, order, limit);
    TEST_ASSERT(select != NULL, "make_select_node failed");
    
    // 打印整个 AST
    char buffer[4096];
    int offset = print_select_node(select, 0, 0, buffer, sizeof(buffer));
    printf("Complex AST:\n%s", buffer);
    
    // 测试 free_ast 递归释放
    free_ast(select);
    TEST_PASS();
    return 0;
}

/* ============================================================
   测试：print_ast 函数
   ============================================================ */
int test_print_ast() {
    printf("测试 print_ast 函数...\n");
    
    // 构建一个简单的 AST
    ASTNode* columns = create_list(make_ident_node("*"), LIST_COLUMN);
    ASTNode* select = make_select_node("users", columns, NULL, NULL, NULL);
    
    // 使用 print_ast
    printf("\nprint_ast output:\n");
    print_ast(select, 2);
    
    free_ast(select);
    TEST_PASS();
    return 0;
}

/* ============================================================
   主函数
   ============================================================ */
int main() {
    printf("╔══════════════════════════════════════════╗\n");
    printf("║     AST 全面测试套件                   ║\n");
    printf("╚══════════════════════════════════════════╝\n");
    
    RUN_TEST(basic_nodes);
    RUN_TEST(list_operations);
    RUN_TEST(select_node);
    RUN_TEST(insert_node);
    RUN_TEST(update_node);
    RUN_TEST(delete_node);
    RUN_TEST(condition_nodes);
    RUN_TEST(ddl_nodes);
    RUN_TEST(order_limit_nodes);
    RUN_TEST(node_lifetime);
    RUN_TEST(complex_ast);
    RUN_TEST(print_ast);
    
    printf("\n========================================\n");
    printf("测试报告:\n");
    printf("  总测试: %d\n", g_test_count);
    printf("  通过: %d\n", g_pass_count);
    printf("  失败: %d\n", g_test_count - g_pass_count);
    printf("========================================\n");
    
    return 0;
}