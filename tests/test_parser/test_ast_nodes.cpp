// tests/test_parser/test_ast_nodes.cpp
//
// 由原 tests/test_ast.cpp 移植：AST 节点构造、列表、打印、生命周期与递归释放。
#include "test_framework.h"

#include "parser/ast.h"

#include <cstring>
#include <string>

namespace {

// 打印器统一签名：返回写入后的 offset
int print_node(ASTNode* node, char* buffer, size_t size) {
    switch (node->type) {
        case NODE_NUMBER:
            return print_number_node(node, 0, 0, buffer, size);
        case NODE_STRING:
            return print_string_node(node, 0, 0, buffer, size);
        case NODE_IDENT:
            return print_ident_node(node, 0, 0, buffer, size);
        case NODE_LITERAL:
            return print_literal_node(node, 0, 0, buffer, size);
        default:
            return 0;
    }
}

}  // namespace

TEST(AstNodes, BasicNodeConstruction) {
    ASTNode* num = make_number_node(42);
    CHECK(num != nullptr);
    CHECK(num->type == NODE_NUMBER);
    CHECK_EQ(((NumberNode*)num->data)->value, 42);

    ASTNode* str = make_string_node("hello");
    CHECK(str != nullptr);
    CHECK(str->type == NODE_STRING);
    CHECK_STREQ(((StringNode*)str->data)->value, "hello");

    ASTNode* ident = make_ident_node("users");
    CHECK(ident != nullptr);
    CHECK(ident->type == NODE_IDENT);
    CHECK_STREQ(((IdentNode*)ident->data)->name, "users");

    char buffer[1024];
    CHECK(print_node(num, buffer, sizeof(buffer)) > 0);
    CHECK(print_node(str, buffer, sizeof(buffer)) > 0);
    CHECK(print_node(ident, buffer, sizeof(buffer)) > 0);

    free_ast(num);
    free_ast(str);
    free_ast(ident);
}

TEST(AstNodes, LiteralNodeDistinguishesNullFromEmptyString) {
    ASTNode* null_literal = make_literal_node(LITERAL_NULL);
    ASTNode* true_literal = make_literal_node(LITERAL_TRUE);
    ASTNode* false_literal = make_literal_node(LITERAL_FALSE);

    CHECK(null_literal->type == NODE_LITERAL);
    CHECK(((LiteralNode*)null_literal->data)->kind == LITERAL_NULL);
    CHECK(((LiteralNode*)true_literal->data)->kind == LITERAL_TRUE);
    CHECK(((LiteralNode*)false_literal->data)->kind == LITERAL_FALSE);

    char buffer[128];
    CHECK(std::string(buffer, (size_t)print_literal_node(null_literal, 0, 0,
                                                        buffer, sizeof(buffer)))
              .find("NULL") != std::string::npos);

    free_ast(null_literal);
    free_ast(true_literal);
    free_ast(false_literal);
}

TEST(AstNodes, ListOperations) {
    ASTNode* item1 = make_ident_node("id");
    ASTNode* item2 = make_ident_node("name");
    ASTNode* item3 = make_ident_node("age");

    ASTNode* list = create_list(item1, LIST_COLUMN);
    CHECK(list != nullptr);
    CHECK(list->type == NODE_LIST);

    auto* data = (ASTNodeList*)list->data;
    CHECK_EQ(data->count, 1);
    CHECK(data->head == item1);
    CHECK(data->tail == item1);

    list = append_to_list(list, item2);
    CHECK_EQ(data->count, 2);
    CHECK(data->head == item1);
    CHECK(data->tail == item2);
    CHECK(item1->next == item2);

    list = append_to_list(list, item3);
    CHECK_EQ(data->count, 3);
    CHECK(data->tail == item3);
    CHECK(item2->next == item3);

    char buffer[1024];
    CHECK(print_list(list, 0, 0, buffer, sizeof(buffer)) > 0);

    free_ast(list);
}

TEST(AstNodes, SelectNodeConstruction) {
    ASTNode* columns = create_list(make_ident_node("*"), LIST_COLUMN);
    ASTNode* select = make_select_node("users", columns, nullptr, nullptr, nullptr);
    CHECK(select != nullptr);
    CHECK(select->type == NODE_SELECT);

    auto* data = (SelectNode*)select->data;
    CHECK_STREQ(data->table, "users");
    CHECK(data->columns != nullptr);
    CHECK(data->condition == nullptr);
    CHECK(data->order_by == nullptr);
    CHECK(data->limit == nullptr);
    free_ast(select);

    // 带 WHERE
    ASTNode* cond = make_compare_node("age", OP_GT, make_number_node(18));
    columns = create_list(make_ident_node("id"), LIST_COLUMN);
    columns = append_to_list(columns, make_ident_node("name"));
    select = make_select_node("users", columns, cond, nullptr, nullptr);
    CHECK(((SelectNode*)select->data)->condition != nullptr);
    CHECK(((SelectNode*)select->data)->condition->type == NODE_COMPARE);
    free_ast(select);

    // 带 ORDER BY
    select = make_select_node("users", nullptr, nullptr,
                              make_order_node("id", OP_ASC), nullptr);
    CHECK(((SelectNode*)select->data)->order_by != nullptr);
    free_ast(select);

    // 带 LIMIT
    ASTNode* limit = make_limit_node(10, 0);
    select = make_select_node("users", nullptr, nullptr, nullptr, limit);
    auto* limit_data = (LimitNode*)limit->data;
    CHECK_EQ(limit_data->limit, 10);
    CHECK_EQ(limit_data->offset, 0);
    char buffer[2048];
    CHECK(print_select_node(select, 0, 0, buffer, sizeof(buffer)) > 0);
    free_ast(select);
}

TEST(AstNodes, InsertNodeConstruction) {
    ASTNode* columns = create_list(make_ident_node("id"), LIST_COLUMN);
    columns = append_to_list(columns, make_ident_node("name"));

    ASTNode* values = create_list(make_number_node(1), LIST_VALUE);
    values = append_to_list(values, make_string_node("Alice"));

    ASTNode* insert = make_insert_node("users", columns, values);
    CHECK(insert != nullptr);
    CHECK(insert->type == NODE_INSERT);

    auto* data = (InsertNode*)insert->data;
    CHECK_STREQ(data->table, "users");
    CHECK(((ASTNodeList*)data->columns->data)->count == 2);
    CHECK(((ASTNodeList*)data->values->data)->count == 2);

    char buffer[2048];
    CHECK(print_insert_node(insert, 0, 0, buffer, sizeof(buffer)) > 0);
    free_ast(insert);
}

TEST(AstNodes, UpdateNodeConstruction) {
    ASTNode* assignments = create_list(
        make_assignment_node("name", make_string_node("Jane")), LIST_ASSIGNMENT);
    assignments = append_to_list(
        assignments, make_assignment_node("age", make_number_node(30)));

    ASTNode* cond = make_compare_node("id", OP_EQ, make_number_node(1));
    ASTNode* update = make_update_node("users", assignments, cond);
    CHECK(update != nullptr);
    CHECK(update->type == NODE_UPDATE);

    auto* data = (UpdateNode*)update->data;
    CHECK_STREQ(data->table, "users");
    CHECK(((ASTNodeList*)data->assignments->data)->count == 2);
    CHECK(data->condition != nullptr);

    char buffer[2048];
    CHECK(print_update_node(update, 0, 0, buffer, sizeof(buffer)) > 0);
    free_ast(update);
}

TEST(AstNodes, DeleteNodeConstruction) {
    ASTNode* cond = make_compare_node("id", OP_EQ, make_number_node(1));
    ASTNode* del = make_delete_node("users", cond);
    CHECK(del != nullptr);
    CHECK(del->type == NODE_DELETE);
    CHECK_STREQ(((DeleteNode*)del->data)->table, "users");
    CHECK(((DeleteNode*)del->data)->condition != nullptr);

    char buffer[1024];
    CHECK(print_delete_node(del, 0, 0, buffer, sizeof(buffer)) > 0);
    free_ast(del);
}

TEST(AstNodes, ConditionNodes) {
    // COMPARE
    ASTNode* compare = make_compare_node("age", OP_GT, make_number_node(18));
    CHECK(compare->type == NODE_COMPARE);
    auto* cmp = (CompareNode*)compare->data;
    CHECK_STREQ(cmp->column, "age");
    CHECK(cmp->op == OP_GT);
    CHECK(cmp->right != nullptr);
    free_ast(compare);

    // IN / NOT IN（各自持有独立的列表：节点所有权是唯一的）
    auto make_number_list = [] {
        ASTNode* list = create_list(make_number_node(1), LIST_VALUE);
        list = append_to_list(list, make_number_node(2));
        list = append_to_list(list, make_number_node(3));
        return list;
    };

    ASTNode* in_node = make_in_node("id", make_number_list());
    auto* in_data = (InNode*)in_node->data;
    CHECK_STREQ(in_data->column, "id");
    CHECK_EQ(in_data->excluded, 0);
    CHECK(in_data->values != nullptr);

    ASTNode* not_in = make_not_in_node("id", make_number_list());
    CHECK(((InNode*)not_in->data)->excluded == 1);

    free_ast(in_node);
    free_ast(not_in);

    // AND
    ASTNode* left = make_compare_node("age", OP_GT, make_number_node(18));
    ASTNode* right = make_compare_node("status", OP_EQ, make_string_node("active"));
    ASTNode* binary = make_binary_node(left, OP_AND, right);
    CHECK(binary->type == NODE_BINARY_OP);
    auto* bin = (BinaryOpNode*)binary->data;
    CHECK(bin->op == OP_AND);
    CHECK(bin->left != nullptr && bin->right != nullptr);

    char buffer[2048];
    CHECK(print_binary_node(binary, 0, 0, buffer, sizeof(buffer)) > 0);
    free_ast(binary);

    // NOT
    ASTNode* not_node = make_not_node(make_compare_node("x", OP_EQ, make_number_node(1)));
    CHECK(not_node->type == NODE_NOT);
    CHECK(((NotNode*)not_node->data)->child != nullptr);
    free_ast(not_node);
}

TEST(AstNodes, DdlNodes) {
    // CREATE TABLE users (id INT PRIMARY KEY, name VARCHAR(32) NOT NULL, age INT)
    ASTNode* columns = create_list(
        make_column_def_node("id", DT_INT, 0, 1, 0), LIST_COLUMN_DEF);
    columns = append_to_list(
        columns, make_column_def_node("name", DT_VARCHAR, 32, 0, 0));
    columns = append_to_list(
        columns, make_column_def_node("age", DT_INT, 0, 0, 1));

    ASTNode* create = make_create_table_node("users", columns);
    CHECK(create != nullptr);
    CHECK(create->type == NODE_CREATE_TABLE);

    auto* create_data = (CreateTableNode*)create->data;
    CHECK_STREQ(create_data->table_name, "users");
    auto* col_list = (ASTNodeList*)create_data->columns->data;
    CHECK_EQ(col_list->count, 3);

    // 列定义的三个字段
    auto* col0 = (ColumnDefNode*)col_list->head->data;
    auto* col1 = (ColumnDefNode*)col_list->head->next->data;
    auto* col2 = (ColumnDefNode*)col_list->head->next->next->data;
    CHECK(col0->data_type == DT_INT);
    CHECK_EQ(col0->is_primary_key, 1);
    CHECK_EQ(col0->nullable, 0);
    CHECK(col1->data_type == DT_VARCHAR);
    CHECK_EQ(col1->length, 32u);
    CHECK_EQ(col1->nullable, 0);
    CHECK_EQ(col2->nullable, 1);

    char buffer[2048];
    CHECK(print_create_table_node(create, 0, 0, buffer, sizeof(buffer)) > 0);
    // 打印里要体现声明长度
    CHECK(std::string(buffer).find("VARCHAR(32)") != std::string::npos);
    free_ast(create);

    ASTNode* drop = make_drop_table_node("users");
    CHECK(drop->type == NODE_DROP_TABLE);
    CHECK_STREQ(((DropTableNode*)drop->data)->table_name, "users");
    free_ast(drop);

    ASTNode* create_db = make_create_database_node("testdb");
    CHECK(create_db->type == NODE_CREATE_DATABASE);
    free_ast(create_db);

    ASTNode* drop_db = make_drop_database_node("testdb");
    CHECK(drop_db->type == NODE_DROP_DATABASE);
    free_ast(drop_db);

    ASTNode* use = make_use_node("testdb");
    CHECK(use->type == NODE_USE);
    CHECK_STREQ(((DatabaseNode*)use->data)->db_name, "testdb");
    free_ast(use);
}

TEST(AstNodes, OrderAndLimitNodes) {
    ASTNode* order_asc = make_order_node("id", OP_ASC);
    CHECK_STREQ(((OrderNode*)order_asc->data)->column, "id");
    CHECK(((OrderNode*)order_asc->data)->direction == OP_ASC);
    free_ast(order_asc);

    ASTNode* order_desc = make_order_node("name", OP_DESC);
    CHECK(((OrderNode*)order_desc->data)->direction == OP_DESC);
    free_ast(order_desc);

    ASTNode* limit = make_limit_node(10, 20);
    auto* limit_data = (LimitNode*)limit->data;
    CHECK_EQ(limit_data->limit, 10);
    CHECK_EQ(limit_data->offset, 20);
    char buffer[1024];
    CHECK(print_limit_node(limit, 0, 0, buffer, sizeof(buffer)) > 0);
    free_ast(limit);
}

TEST(AstNodes, NodeLifetimeRegistryCoversEveryNodeType) {
    for (int i = 0; i < (int)NODE_TYPE_COUNT; ++i) {
        NodeLifetime* lt = get_node_lifetime((NodeType)i);
        CHECK(lt != nullptr);
        if (lt == nullptr) {
            continue;
        }
        CHECK((int)lt->type == i);
        CHECK(lt->do_free != nullptr);
    }
    // 越界
    CHECK(get_node_lifetime((NodeType)999) == nullptr);
}

TEST(AstNodes, ComplexAstBuildsAndFreesRecursively) {
    // SELECT id, name FROM users
    //   WHERE age > 18 AND status = 'active'
    //   ORDER BY id DESC LIMIT 10 OFFSET 5
    ASTNode* columns = create_list(make_ident_node("id"), LIST_COLUMN);
    columns = append_to_list(columns, make_ident_node("name"));

    ASTNode* cond1 = make_compare_node("age", OP_GT, make_number_node(18));
    ASTNode* cond2 = make_compare_node("status", OP_EQ,
                                       make_string_node("active"));
    ASTNode* condition = make_binary_node(cond1, OP_AND, cond2);
    ASTNode* order = make_order_node("id", OP_DESC);
    ASTNode* limit = make_limit_node(10, 5);

    ASTNode* select = make_select_node("users", columns, condition, order, limit);
    CHECK(select != nullptr);

    char buffer[4096];
    const int offset = print_select_node(select, 0, 0, buffer, sizeof(buffer));
    CHECK(offset > 0);

    // 递归释放（配合 ASan/valgrind 可查泄漏）
    free_ast(select);
}

TEST(AstNodes, PrintAstSmoke) {
    ASTNode* columns = create_list(make_ident_node("*"), LIST_COLUMN);
    ASTNode* select = make_select_node("users", columns, nullptr, nullptr, nullptr);
    // print_ast 直接打到 stdout（ctest 只在失败时回显），这里只做冒烟
    print_ast(select, 2);
    CHECK(select != nullptr);
    free_ast(select);
}
