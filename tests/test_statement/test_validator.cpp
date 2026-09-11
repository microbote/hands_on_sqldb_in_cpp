// tests/test_statement/test_validator.cpp
//
// StatementValidator：表/列存在性、值类型与范围、DDL 语义校验。
//
// 接口用法：validate() 返回 std::expected<void, StmtError>，
// 错误码用于判断、message 带上下文，validator 本身无状态可反复使用。
#include "test_framework.h"

#include "memory_catalog.h"
#include "parser/parser.h"
#include "statement/stmt_builder.h"
#include "statement/stmt_validator.h"

#include <string>
#include <vector>

namespace {

MemoryCatalog make_catalog_with_users() {
  MemoryCatalog catalog;
  catalog.set_open(true);
  catalog.create_database(sql::Identifier("shop"));
  catalog.use_database(sql::Identifier("shop"));
  catalog.create_table(sql::Identifier("shop"), make_users_schema());
  return catalog;
}

// 走完整链路：解析 -> 构建 -> 校验，返回错误值（OK 表示通过）
stmt::StmtError validate_sql(const sql::Catalog &catalog,
                             const std::string &sql) {
  parser::Parser parser;
  auto parsed = parser.parse(sql);
  if (!parsed.success) {
    return stmt::StmtError(stmt::StmtErrorCode::AST_IS_NULL, "parse failed");
  }
  stmt::StatementBuilder builder;
  auto query = builder.build(parsed.ast.get());
  if (!query.has_value()) {
    return query.error();
  }
  stmt::StatementValidator validator(catalog);
  auto status = validator.validate(*query);
  return status.has_value() ? stmt::StmtError() : status.error();
}

// 断言：校验失败且错误码匹配
void expect_error(const sql::Catalog &catalog, const std::string &sql,
                  stmt::StmtErrorCode code) {
  const auto error = validate_sql(catalog, sql);
  CHECK(error.code == code);
  if (error.code != code) {
    // 失败时把 message 带进报告
    CHECK_EQ(error.to_string(),
             std::string("<expected ") + stmt::stmt_error_message(code) + ">");
  }
}

// 断言：校验通过
void expect_ok(const sql::Catalog &catalog, const std::string &sql) {
  const auto error = validate_sql(catalog, sql);
  CHECK(error.ok());
  if (!error.ok()) {
    CHECK_EQ(error.to_string(), std::string("<expected OK>"));
  }
}

} // namespace

TEST(Validator, ClosedCatalogIsRejectedOnEveryCall) {
  MemoryCatalog catalog; // 没打开
  catalog.set_open(false);
  catalog.create_database(sql::Identifier("shop"));

  stmt::StatementValidator validator(catalog);

  parser::Parser parser;
  auto parsed = parser.parse("USE shop;");
  CHECK(parsed.success);
  stmt::StatementBuilder builder;
  auto query = builder.build(parsed.ast.get());
  CHECK(query.has_value());
  if (!query.has_value()) {
    return;
  }

  // 校验本身是可重复调用的：每次都返回 VALIDATOR_NOT_INIT，而不是"粘住"状态
  for (int i = 0; i < 2; ++i) {
    auto status = validator.validate(*query);
    CHECK(!status.has_value());
    CHECK(status.error().code == stmt::StmtErrorCode::VALIDATOR_NOT_INIT);
  }
}

TEST(Validator, ReuseAfterFailureStillWorks) {
  // 同一个 validator 实例连续使用：第一次失败不应影响第二次
  auto catalog = make_catalog_with_users();
  expect_error(catalog, "SELECT * FROM nosuchtable;",
               stmt::StmtErrorCode::TABLE_NOT_FOUND);
  expect_ok(catalog, "SELECT * FROM users;");
}

TEST(Validator, UseDatabase) {
  MemoryCatalog catalog;
  catalog.set_open(true);
  catalog.create_database(sql::Identifier("shop"));

  expect_ok(catalog, "USE shop;");
  expect_error(catalog, "USE nosuchdb;",
               stmt::StmtErrorCode::DATABASE_NOT_FOUND);

  const auto error = validate_sql(catalog, "USE nosuchdb;");
  CHECK(!error.message.empty());
}

TEST(Validator, CreateAndDropDatabase) {
  MemoryCatalog catalog;
  catalog.set_open(true);
  catalog.create_database(sql::Identifier("shop"));

  expect_error(catalog, "CREATE DATABASE shop;",
               stmt::StmtErrorCode::DATABASE_ALREADY_EXISTS);
  expect_ok(catalog, "CREATE DATABASE other;");

  // validator 只校验、不执行：先手动建好再校验 DROP
  CHECK(catalog.create_database(sql::Identifier("other")));
  expect_ok(catalog, "DROP DATABASE other;");
  expect_error(catalog, "DROP DATABASE nosuchdb;",
               stmt::StmtErrorCode::DATABASE_NOT_FOUND);
}

TEST(Validator, CreateTableRules) {
  auto catalog = make_catalog_with_users();

  expect_error(catalog, "CREATE TABLE users (id INT PRIMARY KEY);",
               stmt::StmtErrorCode::TABLE_ALREADY_EXISTS);
  expect_ok(catalog, "CREATE TABLE orders (id INT PRIMARY KEY, amount INT);");
  expect_error(catalog, "CREATE TABLE nopk (a INT);",
               stmt::StmtErrorCode::NO_PRIMARY_KEY);
  expect_error(catalog, "CREATE TABLE dup (id INT PRIMARY KEY, a INT, a INT);",
               stmt::StmtErrorCode::DUPLICATE_COLUMN);
  expect_error(catalog,
               "CREATE TABLE twopk (a INT PRIMARY KEY, b INT PRIMARY KEY);",
               stmt::StmtErrorCode::DUPLICATE_PRIMARY_KEY);
  // 非法长度声明在解析期就被拦住
  expect_error(catalog, "CREATE TABLE bad (a CHAR(300) PRIMARY KEY);",
               stmt::StmtErrorCode::AST_IS_NULL);
}

TEST(Validator, DropTableRules) {
  auto catalog = make_catalog_with_users();
  expect_ok(catalog, "DROP TABLE users;");

  CHECK(catalog.drop_table(sql::Identifier("shop"), sql::Identifier("users")));
  expect_error(catalog, "DROP TABLE users;",
               stmt::StmtErrorCode::TABLE_NOT_FOUND);
}

TEST(Validator, SelectChecksTablesAndColumns) {
  auto catalog = make_catalog_with_users();

  expect_ok(catalog, "SELECT * FROM users;");
  expect_ok(catalog, "SELECT id, name FROM users;");
  expect_ok(catalog, "SELECT * FROM users WHERE age > 18;");
  expect_ok(catalog, "SELECT * FROM users WHERE age > 18 ORDER BY name;");

  expect_error(catalog, "SELECT * FROM orders;",
               stmt::StmtErrorCode::TABLE_NOT_FOUND);
  expect_error(catalog, "SELECT nosuchcol FROM users;",
               stmt::StmtErrorCode::COLUMN_NOT_FOUND);
  expect_error(catalog, "SELECT * FROM users WHERE nosuchcol = 1;",
               stmt::StmtErrorCode::COLUMN_NOT_FOUND);
  expect_error(catalog, "SELECT * FROM users ORDER BY nosuchcol;",
               stmt::StmtErrorCode::COLUMN_NOT_FOUND);
}

TEST(Validator, InsertChecksColumnsTypesAndRanges) {
  auto catalog = make_catalog_with_users();

  expect_ok(catalog,
            "INSERT INTO users (id, name, age) VALUES (1, 'Alice', 30);");
  expect_ok(catalog, "INSERT INTO users VALUES (2, 'Bob', 20);");
  expect_ok(catalog, "INSERT INTO users (id) VALUES (3);");
  expect_ok(catalog, "INSERT INTO users (id, age) VALUES (4, NULL);");

  expect_error(catalog, "INSERT INTO users (id, nosuchcol) VALUES (1, 2);",
               stmt::StmtErrorCode::COLUMN_NOT_FOUND);
  expect_error(catalog, "INSERT INTO users (id, name) VALUES (1, 'A', 3);",
               stmt::StmtErrorCode::COLUMN_COUNT_MISMATCH);
  expect_error(catalog, "INSERT INTO users (id, name) VALUES ('abc', 'A');",
               stmt::StmtErrorCode::COLUMN_TYPE_MISMATCH);
  expect_error(catalog, "INSERT INTO users (id) VALUES (NULL);",
               stmt::StmtErrorCode::COLUMN_ATTR_NULL_MISMATCH);
  expect_error(catalog, "INSERT INTO users (name) VALUES ('A');",
               stmt::StmtErrorCode::COLUMN_ATTR_NULL_MISMATCH);

  // 超出 VARCHAR(32) 的声明长度
  const std::string too_long(33, 'x');
  expect_error(catalog,
               "INSERT INTO users (id, name) VALUES (1, '" + too_long + "');",
               stmt::StmtErrorCode::VALUE_OUT_OF_RANGE);
}

TEST(Validator, IntegerRangeIsCheckedPerColumn) {
  MemoryCatalog catalog;
  catalog.set_open(true);
  catalog.create_database(sql::Identifier("shop"));
  catalog.use_database(sql::Identifier("shop"));

  sql::TableSchema schema(sql::Identifier("tiny"));
  schema.add_column(sql::Identifier("id"), sql::DataType::TINYINT, true, false);
  catalog.create_table(sql::Identifier("shop"), schema);

  expect_ok(catalog, "INSERT INTO tiny VALUES (127);");
  expect_error(catalog, "INSERT INTO tiny VALUES (128);",
               stmt::StmtErrorCode::VALUE_OUT_OF_RANGE);
}

TEST(Validator, UpdateRules) {
  auto catalog = make_catalog_with_users();

  expect_ok(catalog, "UPDATE users SET age = 31 WHERE id = 1;");
  expect_ok(catalog, "UPDATE users SET name = NULL WHERE id = 1;");

  expect_error(catalog, "UPDATE users SET nosuchcol = 1;",
               stmt::StmtErrorCode::COLUMN_NOT_FOUND);
  expect_error(catalog, "UPDATE users SET age = 'abc';",
               stmt::StmtErrorCode::COLUMN_TYPE_MISMATCH);
  expect_error(catalog, "UPDATE users SET id = 9 WHERE id = 1;",
               stmt::StmtErrorCode::INVALID_SCHEMA);
  expect_error(catalog, "UPDATE users SET age = 1 WHERE nosuchcol = 1;",
               stmt::StmtErrorCode::COLUMN_NOT_FOUND);
}

TEST(Validator, DeleteRules) {
  auto catalog = make_catalog_with_users();
  expect_ok(catalog, "DELETE FROM users WHERE id = 1;");
  expect_ok(catalog, "DELETE FROM users;");
  expect_error(catalog, "DELETE FROM users WHERE nosuchcol = 1;",
               stmt::StmtErrorCode::COLUMN_NOT_FOUND);
}

TEST(Validator, NoDatabaseSelected) {
  MemoryCatalog catalog;
  catalog.set_open(true); // 打开但没 USE
  catalog.create_database(sql::Identifier("shop"));

  const auto error = validate_sql(catalog, "SELECT * FROM users;");
  CHECK(error.code == stmt::StmtErrorCode::DATABASE_NOT_FOUND);
  CHECK(error.message.find("no database selected") != std::string::npos);
}

TEST(Validator, DynamicCatalogChangesAreVisible) {
  auto catalog = make_catalog_with_users();
  expect_ok(catalog, "SELECT * FROM users;");

  catalog.drop_table(sql::Identifier("shop"), sql::Identifier("users"));
  expect_error(catalog, "SELECT * FROM users;",
               stmt::StmtErrorCode::TABLE_NOT_FOUND);
}
