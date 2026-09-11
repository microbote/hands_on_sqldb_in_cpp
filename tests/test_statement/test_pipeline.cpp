// tests/test_statement/test_pipeline.cpp
//
// 端到端：SQL 文本 -> Parser -> StatementBuilder -> StatementValidator ->
// 执行(DDL)。
#include "test_framework.h"

#include "memory_catalog.h"
#include "parser/parser.h"
#include "statement/stmt_builder.h"
#include "statement/stmt_validator.h"

#include <string>
#include <vector>

namespace {

struct PipelineResult {
  stmt::StmtError error;  // OK 表示整条链路通过
  std::string query_text; // 调试用：Query::to_string()
  bool ok() const { return error.ok(); }
};

PipelineResult run_pipeline(const sql::Catalog &catalog,
                            const std::string &sql) {
  PipelineResult result;
  parser::Parser parser;
  auto parsed = parser.parse(sql);
  if (!parsed.success) {
    result.error =
        stmt::StmtError(stmt::StmtErrorCode::AST_IS_NULL, "parse failed");
    return result;
  }
  stmt::StatementBuilder builder;
  auto query = builder.build(parsed.ast.get());
  if (!query.has_value()) {
    result.error = query.error();
    return result;
  }
  result.query_text = query->to_string();

  stmt::StatementValidator validator(catalog);
  auto status = validator.validate(*query);
  if (!status.has_value()) {
    result.error = status.error();
  }
  return result;
}

MemoryCatalog make_environment() {
  MemoryCatalog catalog;
  catalog.set_open(true);
  catalog.create_database(sql::Identifier("shop"));
  catalog.use_database(sql::Identifier("shop"));
  catalog.create_table(sql::Identifier("shop"), make_users_schema());
  return catalog;
}

// validator 只校验、不执行；这里模拟执行器把成功的 DDL 落到 catalog 上
void apply_ddl(MemoryCatalog &catalog, const sql::Query &query) {
  if (const auto *create_table = query.create_table()) {
    sql::TableSchema schema(create_table->table);
    for (const auto &column : create_table->columns) {
      schema.add_column(column);
    }
    catalog.create_table(catalog.current_database(), schema);
    return;
  }
  if (const auto *drop_table = query.drop_table()) {
    catalog.drop_table(catalog.current_database(), drop_table->table);
    return;
  }
  if (const auto *create_db = query.create_database()) {
    catalog.create_database(create_db->database);
    return;
  }
  if (const auto *drop_db = query.drop_database()) {
    catalog.drop_database(drop_db->database);
    return;
  }
  if (const auto *use = query.use_database()) {
    catalog.use_database(use->database);
  }
}

// 完整流程：解析 -> 构建 -> 校验 -> 执行
PipelineResult run_and_apply(MemoryCatalog &catalog, const std::string &sql) {
  PipelineResult result;
  parser::Parser parser;
  auto parsed = parser.parse(sql);
  if (!parsed.success) {
    result.error =
        stmt::StmtError(stmt::StmtErrorCode::AST_IS_NULL, "parse failed");
    return result;
  }
  stmt::StatementBuilder builder;
  auto query = builder.build(parsed.ast.get());
  if (!query.has_value()) {
    result.error = query.error();
    return result;
  }
  result.query_text = query->to_string();

  stmt::StatementValidator validator(catalog);
  auto status = validator.validate(*query);
  if (!status.has_value()) {
    result.error = status.error();
    return result;
  }
  apply_ddl(catalog, *query);
  return result;
}

} // namespace

TEST(Pipeline, EndToEndStatementsAreValidated) {
  auto catalog = make_environment();

  const std::vector<std::string> good_sql = {
      "SELECT * FROM users;",
      "SELECT id, name FROM users WHERE age > 18;",
      "SELECT * FROM users WHERE age > 18 AND name IS NOT NULL "
      "ORDER BY age DESC LIMIT 10 OFFSET 20;",
      "SELECT * FROM users WHERE id IN (1, 2, NULL);",
      "SELECT * FROM users WHERE name LIKE 'A%';",
      "INSERT INTO users (id, name, age) VALUES (1, 'Alice', 30);",
      "INSERT INTO users VALUES (2, 'Bob', NULL);",
      "UPDATE users SET age = 31 WHERE id = 1;",
      "DELETE FROM users WHERE id = 1;",
      "CREATE TABLE orders (id INT PRIMARY KEY, amount BIGINT);",
      "DROP TABLE orders;",
      "CREATE DATABASE other;",
      "DROP DATABASE other;",
      "USE shop;",
  };

  for (const auto &sql : good_sql) {
    const auto result = run_and_apply(catalog, sql);
    CHECK(result.ok());
    if (!result.ok()) {
      // 失败时把错误信息带进报告
      CHECK_EQ(result.error.to_string(), std::string("<expected OK>"));
    }
    CHECK(!result.query_text.empty());
  }
}

TEST(Pipeline, SemanticErrorsAreReported) {
  auto catalog = make_environment();

  struct Case {
    const char *sql;
    stmt::StmtErrorCode expected;
  };
  const Case cases[] = {
      {"SELECT * FROM non_existent;", stmt::StmtErrorCode::TABLE_NOT_FOUND},
      {"SELECT invalid_col FROM users;", stmt::StmtErrorCode::COLUMN_NOT_FOUND},
      {"SELECT * FROM users WHERE invalid_col = 1;",
       stmt::StmtErrorCode::COLUMN_NOT_FOUND},
      {"SELECT * FROM users ORDER BY invalid_col;",
       stmt::StmtErrorCode::COLUMN_NOT_FOUND},
      {"INSERT INTO users (id, name) VALUES (1, 'A', 3);",
       stmt::StmtErrorCode::COLUMN_COUNT_MISMATCH},
      {"INSERT INTO users (id) VALUES ('abc');",
       stmt::StmtErrorCode::COLUMN_TYPE_MISMATCH},
      {"INSERT INTO users (id) VALUES (NULL);",
       stmt::StmtErrorCode::COLUMN_ATTR_NULL_MISMATCH},
      {"UPDATE users SET id = 2 WHERE id = 1;",
       stmt::StmtErrorCode::INVALID_SCHEMA},
      {"DELETE FROM users WHERE invalid_col = 1;",
       stmt::StmtErrorCode::COLUMN_NOT_FOUND},
  };

  for (const auto &item : cases) {
    const auto result = run_pipeline(catalog, item.sql);
    CHECK(result.error.code == item.expected);
    CHECK(!result.error.message.empty());
  }
}

TEST(Pipeline, UseSwitchesDatabase) {
  MemoryCatalog catalog;
  catalog.set_open(true);
  catalog.create_database(sql::Identifier("shop"));
  catalog.create_database(sql::Identifier("warehouse"));
  catalog.create_table(sql::Identifier("shop"), make_users_schema());

  // 还没 USE：无法解析表
  const auto before = run_pipeline(catalog, "SELECT * FROM users;");
  CHECK(before.error.code == stmt::StmtErrorCode::DATABASE_NOT_FOUND);

  // USE 之后（run_and_apply 会真的切换当前库）就能查到 shop.users
  CHECK(run_and_apply(catalog, "USE shop;").ok());
  CHECK(catalog.current_database() == "shop");
  CHECK(run_pipeline(catalog, "SELECT * FROM users;").ok());
}

TEST(Pipeline, QueryTextIsAvailableForDebugging) {
  auto catalog = make_environment();
  const auto result = run_pipeline(
      catalog,
      "SELECT id, name FROM users WHERE age > 18 ORDER BY id LIMIT 5;");
  CHECK(result.ok());
  CHECK(result.query_text.find("SELECT") != std::string::npos);
  CHECK(result.query_text.find("users") != std::string::npos);
  CHECK(result.query_text.find("WHERE") != std::string::npos);
  CHECK(result.query_text.find("LIMIT 5") != std::string::npos);
}
