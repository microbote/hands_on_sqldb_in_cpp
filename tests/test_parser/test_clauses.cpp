// tests/test_parser/test_clauses.cpp
//
// 子句相关：ORDER BY 多列（曾经因为 $2 引用逗号 token 而构造出错误链表）、
// LIMIT/OFFSET、IS NULL、IN、LIKE、UPDATE/DELETE。
#include "test_framework.h"

#include "parser/parser.h"

#include <string>

namespace {

parser::ASTNodePtr parse_select(const std::string& tail) {
    parser::Parser p;
    auto result = p.parse("SELECT * FROM t " + tail + ";");
    return result.success ? std::move(result.ast) : nullptr;
}

const ASTNodeList* order_list_of(const ASTNode* select) {
    if (select == nullptr || select->type != NODE_SELECT) {
        return nullptr;
    }
    const auto* data = (const SelectNode*)select->data;
    if (data->order_by == nullptr || data->order_by->type != NODE_LIST) {
        return nullptr;
    }
    return (const ASTNodeList*)data->order_by->data;
}

const OrderNode* order_item_at(const ASTNodeList* list, int index) {
    if (list == nullptr) {
        return nullptr;
    }
    const ASTNode* item = list->head;
    for (int i = 0; i < index && item != nullptr; ++i) {
        item = item->next;
    }
    if (item == nullptr || item->type != NODE_ORDER) {
        return nullptr;
    }
    return (const OrderNode*)item->data;
}

}  // namespace

TEST(Clauses, OrderByMultipleColumnsBuildsCompleteList) {
    auto ast = parse_select("ORDER BY a, b");
    const ASTNodeList* list = order_list_of(ast.get());
    CHECK(list != nullptr);
    if (list == nullptr) {
        return;
    }
    // $2/$3 bug：这里以前会把逗号当节点，count 与内容都不对
    CHECK_EQ(list->count, 2);
    const OrderNode* first = order_item_at(list, 0);
    const OrderNode* second = order_item_at(list, 1);
    CHECK(first != nullptr && second != nullptr);
    if (first != nullptr) {
        CHECK_EQ(std::string(first->column), std::string("a"));
        CHECK(first->direction == OP_ASC);
    }
    if (second != nullptr) {
        CHECK_EQ(std::string(second->column), std::string("b"));
        CHECK(second->direction == OP_ASC);
    }
}

TEST(Clauses, OrderByDirectionIsPreserved) {
    auto ast = parse_select("ORDER BY a DESC, b ASC, c");
    const ASTNodeList* list = order_list_of(ast.get());
    CHECK(list != nullptr);
    if (list == nullptr) {
        return;
    }
    CHECK_EQ(list->count, 3);
    CHECK(order_item_at(list, 0)->direction == OP_DESC);
    CHECK(order_item_at(list, 1)->direction == OP_ASC);
    CHECK(order_item_at(list, 2)->direction == OP_ASC);
}

TEST(Clauses, LimitAndOffsetVariants) {
    parser::Parser p;
    CHECK(p.parse("SELECT * FROM t LIMIT 10;").success);
    CHECK(p.parse("SELECT * FROM t LIMIT 10 OFFSET 5;").success);
    CHECK(p.parse("SELECT * FROM t LIMIT 5, 10;").success);
    CHECK(p.parse("SELECT * FROM t ORDER BY a, b LIMIT 3;").success);
}

TEST(Clauses, NullCompareAndSetMembership) {
    parser::Parser p;
    CHECK(p.parse("SELECT * FROM t WHERE name IS NULL;").success);
    CHECK(p.parse("SELECT * FROM t WHERE name IS NOT NULL;").success);
    CHECK(p.parse("SELECT * FROM t WHERE id IN (1, 2, NULL);").success);
    CHECK(p.parse("SELECT * FROM t WHERE id NOT IN (1, 2);").success);
    CHECK(p.parse("SELECT * FROM t WHERE name LIKE 'a%';").success);
    CHECK(p.parse("SELECT * FROM t WHERE name NOT LIKE 'a%';").success);
    CHECK(p.parse("SELECT * FROM t WHERE a > 1 AND b < 2 OR NOT c = 3;").success);
}

TEST(Clauses, UpdateAndDelete) {
    parser::Parser p;
    CHECK(p.parse("UPDATE t SET a = 1, b = 'x' WHERE id = 1;").success);
    CHECK(p.parse("UPDATE t SET a = NULL;").success);
    CHECK(p.parse("UPDATE t SET a = TRUE, b = -3;").success);
    CHECK(p.parse("DELETE FROM t WHERE id = 1;").success);
    CHECK(p.parse("DELETE FROM t;").success);
}
