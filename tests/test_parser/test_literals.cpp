// tests/test_parser/test_literals.cpp
//
// 字面量：整数(int64)、负数、NULL/TRUE/FALSE、字符串 '' 转义。
#include "test_framework.h"

#include "parser/parser.h"

#include <string>

namespace {

parser::ASTNodePtr parse_insert(const std::string& values) {
    parser::Parser p;
    auto result = p.parse("INSERT INTO t VALUES (" + values + ");");
    return result.success ? std::move(result.ast) : nullptr;
}

const ASTNode* first_value(const ASTNode* insert) {
    if (insert == nullptr || insert->type != NODE_INSERT) {
        return nullptr;
    }
    const auto* data = (const InsertNode*)insert->data;
    if (data->values == nullptr) {
        return nullptr;
    }
    const auto* list = (const ASTNodeList*)data->values->data;
    return list->head;
}

int64_t number_value(const ASTNode* insert) {
    const ASTNode* node = first_value(insert);
    if (node == nullptr || node->type != NODE_NUMBER) {
        return 0;
    }
    return ((const NumberNode*)node->data)->value;
}

const char* string_value(const ASTNode* insert) {
    const ASTNode* node = first_value(insert);
    if (node == nullptr || node->type != NODE_STRING) {
        return nullptr;
    }
    return ((const StringNode*)node->data)->value;
}

LiteralKind literal_kind(const ASTNode* insert) {
    const ASTNode* node = first_value(insert);
    if (node == nullptr || node->type != NODE_LITERAL) {
        return (LiteralKind)-1;
    }
    return ((const LiteralNode*)node->data)->kind;
}

}  // namespace

TEST(Literals, IntegerLiteralsKeepFullInt64Range) {
    CHECK_EQ(number_value(parse_insert("1").get()), 1);
    CHECK_EQ(number_value(parse_insert("0").get()), 0);
    // 之前用 atoi + int，3000000000 会变成 -1294967296
    CHECK_EQ(number_value(parse_insert("3000000000").get()), 3000000000LL);
    CHECK_EQ(number_value(parse_insert("9223372036854775807").get()),
             INT64_MAX);
}

TEST(Literals, NegativeLiterals) {
    // 之前 '-' 会被词法静默丢弃，-5 变成 5
    CHECK_EQ(number_value(parse_insert("-5").get()), -5);
    CHECK_EQ(number_value(parse_insert("-9223372036854775807").get()),
             -9223372036854775807LL);
}

TEST(Literals, OutOfRangeIntegerIsRejected) {
    CHECK(parse_insert("99999999999999999999") == nullptr);
    CHECK(parse_insert("9223372036854775808") == nullptr);   // INT64_MAX + 1
}

TEST(Literals, NullTrueFalseAreDistinctNodes) {
    CHECK(literal_kind(parse_insert("NULL").get()) == LITERAL_NULL);
    CHECK(literal_kind(parse_insert("null").get()) == LITERAL_NULL);
    CHECK(literal_kind(parse_insert("TRUE").get()) == LITERAL_TRUE);
    CHECK(literal_kind(parse_insert("FALSE").get()) == LITERAL_FALSE);

    // NULL 与空串必须是不同节点：sql_types 里 NULL != ''
    auto null_ast = parse_insert("NULL");
    auto empty_ast = parse_insert("''");
    CHECK(first_value(null_ast.get())->type == NODE_LITERAL);
    CHECK(first_value(empty_ast.get())->type == NODE_STRING);
    CHECK_EQ(std::string(string_value(empty_ast.get())), std::string(""));
}

TEST(Literals, StringEscapesDoubleSingleQuote) {
    CHECK_EQ(std::string(string_value(parse_insert("'hello'").get())),
             std::string("hello"));
    CHECK_EQ(std::string(string_value(parse_insert("'it''s'").get())),
             std::string("it's"));
    CHECK_EQ(std::string(string_value(parse_insert("''").get())), std::string(""));
    CHECK_EQ(std::string(string_value(parse_insert("'a''''b'").get())),
             std::string("a''b"));
}

TEST(Literals, InsertAcceptsNullInColumnList) {
    parser::Parser p;
    auto result = p.parse("INSERT INTO t (a, b) VALUES (1, NULL);");
    CHECK(result.success);
}

TEST(Literals, InListAcceptsNull) {
    parser::Parser p;
    auto result = p.parse("SELECT * FROM t WHERE id IN (1, NULL);");
    CHECK(result.success);
}

TEST(Literals, LimitAndOffsetStillWork) {
    parser::Parser p;
    CHECK(p.parse("SELECT * FROM t LIMIT 10;").success);
    CHECK(p.parse("SELECT * FROM t LIMIT 10 OFFSET 5;").success);
    CHECK(p.parse("SELECT * FROM t LIMIT 5, 10;").success);
    // 超出 int 范围的 LIMIT 应该报错而不是截断
    CHECK(!p.parse("SELECT * FROM t LIMIT 99999999999;").success);
}
