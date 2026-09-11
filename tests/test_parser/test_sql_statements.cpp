// tests/test_parser/test_sql_statements.cpp
//
// 由原 tests/test_parser.cpp 移植：各类 SQL 语句的解析、C API 路径、
// 多次解析的状态重置，以及 Parser 门面（ParseResult / parse_multi）。
#include "test_framework.h"

#include "parser/ast.h"
#include "parser/parser.h"

#include <string>
#include <vector>

// C API（main.cpp 走的就是这条路径）
extern "C" {
#include <lex.yy.h>
#include <parser.tab.h>
extern int yy_flex_debug;
}

namespace {

// 用 C API 解析一条 SQL，返回是否成功；成功后释放 AST
bool parse_via_c_api(const std::string& sql) {
    YY_BUFFER_STATE buffer = yy_scan_string(sql.c_str());
    const int result = yyparse();
    yy_delete_buffer(buffer);

    const bool ok = (result == 0 && g_parsed_ast != nullptr);
    reset_parser();   // 释放 AST 并清空全局状态
    return ok;
}

// 用 Parser 门面解析
bool parse_ok(parser::Parser& parser, const std::string& sql) {
    return parser.parse(sql).success;
}

bool parse_ok(const std::string& sql) {
    parser::Parser parser;
    return parse_ok(parser, sql);
}

}  // namespace

TEST(SqlStatements, BasicSelectAndUse) {
    CHECK(parse_ok("USE testdb;"));
    CHECK(parse_ok("SELECT * FROM users;"));
    CHECK(parse_ok("SELECT id, name FROM users;"));
    CHECK(parse_ok("SELECT id, name, age FROM users;"));

    CHECK(parse_ok("SELECT * FROM users WHERE age > 18;"));
    CHECK(parse_ok("SELECT id, name FROM users WHERE age >= 18;"));
    CHECK(parse_ok("SELECT * FROM users WHERE age < 65;"));
    CHECK(parse_ok("SELECT * FROM users WHERE age <= 65;"));
    CHECK(parse_ok("SELECT * FROM users WHERE age != 18;"));
    CHECK(parse_ok("SELECT * FROM users WHERE age <> 18;"));
    CHECK(parse_ok("SELECT * FROM users WHERE name = 'Alice';"));
}

TEST(SqlStatements, ComplexConditions) {
    CHECK(parse_ok("SELECT * FROM users WHERE age > 18 AND status = 'active';"));
    CHECK(parse_ok("SELECT * FROM users WHERE age > 18 OR status = 'inactive';"));
    CHECK(parse_ok("SELECT * FROM users WHERE age > 18 AND age < 65;"));
    CHECK(parse_ok("SELECT * FROM users WHERE (age > 18);"));
    CHECK(parse_ok("SELECT * FROM users WHERE age > 18 AND (status = 'active' "
                   "OR status = 'pending');"));
    CHECK(parse_ok("SELECT * FROM users WHERE (age > 18 OR age < 10) AND "
                   "status = 'active';"));
    CHECK(parse_ok("SELECT * FROM products WHERE price > 100 AND "
                   "(category = 'Electronics' OR category = 'Computers');"));
    CHECK(parse_ok("SELECT * FROM products WHERE (price > 100 AND "
                   "(category = 'Electronics' OR category = 'Computers'));"));
}

TEST(SqlStatements, InAndLike) {
    CHECK(parse_ok("SELECT * FROM users WHERE id IN (1, 2, 3);"));
    CHECK(parse_ok("SELECT * FROM users WHERE id NOT IN (1, 2, 3);"));
    CHECK(parse_ok("SELECT * FROM users WHERE id IN (1, 2, 3, 4, 5);"));
    CHECK(parse_ok("SELECT * FROM users WHERE name LIKE 'John%';"));
    CHECK(parse_ok("SELECT * FROM users WHERE name LIKE '%son';"));
    CHECK(parse_ok("SELECT * FROM users WHERE name LIKE '%a%';"));
    CHECK(parse_ok("SELECT * FROM users WHERE name NOT LIKE 'admin%';"));
    // NULL 可以出现在 IN 列表里（sql_types 的三值逻辑需要）
    CHECK(parse_ok("SELECT * FROM users WHERE id IN (1, 2, NULL);"));
}

TEST(SqlStatements, IsNullPredicates) {
    CHECK(parse_ok("SELECT * FROM users WHERE email IS NULL;"));
    CHECK(parse_ok("SELECT * FROM users WHERE email IS NOT NULL;"));
    CHECK(parse_ok(
        "SELECT * FROM users WHERE age IS NULL AND name IS NOT NULL;"));
}

TEST(SqlStatements, OrderByAndLimit) {
    CHECK(parse_ok("SELECT * FROM users ORDER BY id;"));
    CHECK(parse_ok("SELECT * FROM users ORDER BY id ASC;"));
    CHECK(parse_ok("SELECT * FROM users ORDER BY id DESC;"));
    CHECK(parse_ok("SELECT * FROM users ORDER BY name DESC;"));
    CHECK(parse_ok("SELECT * FROM users ORDER BY age DESC, name ASC;"));
    CHECK(parse_ok("SELECT * FROM users LIMIT 10;"));
    CHECK(parse_ok("SELECT * FROM users LIMIT 10 OFFSET 20;"));
    CHECK(parse_ok("SELECT * FROM users ORDER BY id LIMIT 5 OFFSET 10;"));
}

TEST(SqlStatements, Insert) {
    CHECK(parse_ok("INSERT INTO users (id, name) VALUES (1, 'Alice');"));
    CHECK(parse_ok("INSERT INTO users (id, name, age) VALUES (1, 'Bob', 25);"));
    CHECK(parse_ok("INSERT INTO users (id, name, age, email) VALUES "
                   "(1, 'Charlie', 30, 'charlie@test.com');"));
    CHECK(parse_ok("INSERT INTO users (name) VALUES ('Alice');"));
    CHECK(parse_ok("INSERT INTO users VALUES (1, 'David', 28, "
                   "'david@test.com');"));
}

TEST(SqlStatements, Update) {
    CHECK(parse_ok("UPDATE users SET name = 'Jane' WHERE id = 1;"));
    CHECK(parse_ok("UPDATE users SET name = 'Jane', age = 30 WHERE id = 1;"));
    CHECK(parse_ok("UPDATE users SET status = 'inactive' WHERE age > 65;"));
    CHECK(parse_ok("UPDATE users SET name = 'Admin' WHERE id IN (1, 2, 3);"));
}

TEST(SqlStatements, Delete) {
    CHECK(parse_ok("DELETE FROM users WHERE id = 1;"));
    CHECK(parse_ok("DELETE FROM users WHERE age > 65;"));
    CHECK(parse_ok("DELETE FROM users WHERE name LIKE '%test%';"));
    CHECK(parse_ok("DELETE FROM users WHERE id IN (1, 2, 3);"));
    CHECK(parse_ok("DELETE FROM users WHERE age IS NULL;"));
}

TEST(SqlStatements, Ddl) {
    CHECK(parse_ok("USE testdb;"));
    CHECK(parse_ok("CREATE DATABASE testdb;"));
    CHECK(parse_ok("DROP DATABASE testdb;"));
    CHECK(parse_ok("CREATE TABLE users (id INT PRIMARY KEY, name VARCHAR);"));
    CHECK(parse_ok("CREATE TABLE users (id INT PRIMARY KEY NOT NULL, "
                   "name VARCHAR NOT NULL, age INT);"));
    CHECK(parse_ok("CREATE TABLE users (id INT PRIMARY KEY, name VARCHAR NOT "
                   "NULL, age INT NULL);"));
    CHECK(parse_ok("DROP TABLE users;"));
}

TEST(SqlStatements, ComplexStatements) {
    CHECK(parse_ok(
        "SELECT id, name, age FROM users "
        "WHERE age > 18 AND status = 'active' AND email IS NOT NULL "
        "ORDER BY age DESC, name ASC LIMIT 10 OFFSET 20;"));
    CHECK(parse_ok(
        "SELECT * FROM users WHERE id IN (1, 2, 3, 4, 5, 6, 7, 8, 9, 10);"));
    CHECK(parse_ok(
        "SELECT * FROM products WHERE (price > 100 OR price < 10) AND "
        "(category = 'Electronics' OR category = 'Books');"));
    CHECK(parse_ok(
        "SELECT id, name, age, email FROM users "
        "WHERE (age > 18 OR age < 10) AND email IS NOT NULL AND "
        "name LIKE 'A%' ORDER BY age DESC LIMIT 5 OFFSET 0;"));
}

TEST(SqlStatements, InvalidStatementsAreRejected) {
    CHECK(!parse_ok("SELEC * FROM users;"));
    CHECK(!parse_ok("SELECT * FROM;"));
    CHECK(!parse_ok("SELECT * users;"));
    CHECK(!parse_ok("SELECT * FROM users WHERE;"));
    // 缺少列定义是合法的（SQL 允许）
    CHECK(parse_ok("INSERT INTO users VALUES (1, 'Alice');"));
}

TEST(SqlStatements, UnsupportedSyntaxIsRejectedNotSilentlyAccepted) {
    // 算术表达式暂不支持（见 parser/README.md 支持矩阵），必须报错
    CHECK(!parse_ok("UPDATE users SET age = age + 1 WHERE age < 18;"));
    // 双引号标识符暂不支持
    CHECK(!parse_ok("SELECT * FROM \"users\";"));
}

TEST(SqlStatements, RepeatedParsingResetsState) {
    const char* sqls[] = {
        "SELECT * FROM users;",
        "SELECT id, name FROM users WHERE age > 18;",
        "INSERT INTO users (id, name) VALUES (1, 'Alice');",
        "UPDATE users SET name = 'Bob' WHERE id = 1;",
        "DELETE FROM users WHERE id = 1;",
    };
    parser::Parser parser;
    for (const char* sql : sqls) {
        CHECK(parse_ok(parser, sql));
    }
    // 解析完成后没有残留 AST
    CHECK(g_parsed_ast == nullptr);
}

TEST(SqlStatements, CApiPathWorks) {
    // main.cpp 直接使用 C API：yy_scan_string / yyparse / g_parsed_ast / reset_parser
    CHECK(parse_via_c_api("SELECT * FROM users;"));
    CHECK(parse_via_c_api("INSERT INTO users VALUES (1, 'Alice');"));
    CHECK(!parse_via_c_api("SELEC * FROM users;"));
    CHECK(g_parsed_ast == nullptr);
}

TEST(SqlStatements, ParserFacadeOwnsAst) {
    parser::Parser parser;
    auto result = parser.parse("SELECT * FROM users;");
    CHECK(result.success);
    CHECK(result.ast != nullptr);
    if (result.ast != nullptr) {
        CHECK(result.ast->type == NODE_SELECT);
        // ParseResult 析构时通过 free_ast 自动释放
    }
}

TEST(SqlStatements, ParseMultiReturnsOneResultPerStatement) {
    parser::Parser parser;
    auto results = parser.parse_multi(
        "SELECT * FROM users; \n"
        "SELECT id FROM users; \n"
        "INSERT INTO users VALUES (1, 'Alice');\n");

    CHECK_EQ(results.size(), 3u);
    if (results.size() == 3) {
        CHECK(results[0].success && results[0].ast->type == NODE_SELECT);
        CHECK(results[1].success && results[1].ast->type == NODE_SELECT);
        CHECK(results[2].success && results[2].ast->type == NODE_INSERT);
        CHECK(results[0].s_sql().find("SELECT") != std::string::npos);
    }
}

TEST(SqlStatements, MultiStatementWithErrorStillCountsErrors) {
    parser::Parser parser;
    auto results = parser.parse_multi(
        "SELECT * FROM users;\n"
        "SELEC * FROM users;\n");
    CHECK_EQ(results.size(), 2u);
    if (results.size() == 2) {
        CHECK(results[0].success);
        CHECK(!results[1].success);
        CHECK(results[1].error.has_value());
    }
}
