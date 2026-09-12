// tests/test_statement/test_builder.cpp
//
// StatementBuilder：AST -> sql::Query 的转换与结构错误处理。
#include "test_framework.h"

#include "parser/parser.h"
#include "sql_types/query.h"
#include "statement/stmt_builder.h"

#include <string>

namespace {

// 解析 SQL -> 构建 Query。失败时错误码与信息都在 expected 里。
using BuildOutcome = std::expected<sql::Query, stmt::StmtError>;

BuildOutcome build_sql(const std::string &sql) {
  parser::Parser parser;
  auto parsed = parser.parse(sql);
  if (!parsed.success) {
    return std::unexpected(
        stmt::StmtError(stmt::StmtErrorCode::AST_IS_NULL, "parse failed"));
  }
  stmt::StatementBuilder builder;
  return builder.build(parsed.ast.get());
}

// 断言构建成功并把 Query 绑定到一个名字上（失败则本用例直接返回）
#define REQUIRE_QUERY(outcome, name)                                           \
  CHECK((outcome).has_value());                                                \
  if (!(outcome).has_value()) {                                                \
    return;                                                                    \
  }                                                                            \
  const sql::Query &name = *(outcome)

} // namespace

TEST(Builder, BuildNullAstFails) {
  stmt::StatementBuilder builder;
  auto result = builder.build(nullptr);
  CHECK(!result.has_value());
  CHECK(result.error().code == stmt::StmtErrorCode::AST_IS_NULL);
  CHECK(!result.error().message.empty());
  CHECK(!result.error().ok());
}

TEST(Builder, BuildSelectQuery) {
  auto outcome = build_sql(
      "SELECT id, name FROM users WHERE age > 18 AND name IS NOT NULL "
      "ORDER BY age DESC, name ASC LIMIT 10 OFFSET 5;");
  REQUIRE_QUERY(outcome, query);

  CHECK(query.is_select());
  const auto *select = query.select();
  CHECK(select != nullptr);
  if (select == nullptr) {
    return;
  }
  CHECK(select->table == "users");
  CHECK(select->table == "USERS"); // Identifier 大小写不敏感
  CHECK_EQ(select->columns.size(), 2u);
  CHECK(select->columns[0].column == "id");
  CHECK(select->columns[1].column == "name");
  CHECK(!select->select_all());
  CHECK(select->where != nullptr);
  CHECK_EQ(select->order_by.size(), 2u);
  CHECK(select->order_by[0].direction == sql::OrderDirection::DESC);
  CHECK(select->order_by[1].direction == sql::OrderDirection::ASC);
  CHECK(select->limit.has_limit());
  CHECK_EQ(select->limit.limit_value(), 10u);
  CHECK(select->limit.has_offset());
  CHECK_EQ(select->limit.offset_value(), 5u);
}

TEST(Builder, BuildSelectAllAndOrderByWithoutClauses) {
  auto outcome = build_sql("SELECT * FROM users;");
  REQUIRE_QUERY(outcome, query);
  const auto *select = query.select();
  CHECK(select != nullptr);
  if (select == nullptr) {
    return;
  }
  CHECK(select->select_all());
  CHECK(select->where == nullptr);
  CHECK(select->order_by.empty());
  CHECK(!select->limit.has_limit());
}

TEST(Builder, BuildInsertQueryWithLiterals) {
  auto outcome =
      build_sql("INSERT INTO users (id, name, age) VALUES (1, 'Alice', NULL);");
  REQUIRE_QUERY(outcome, query);

  CHECK(query.is_insert());
  const auto *insert = query.insert();
  CHECK(insert != nullptr);
  if (insert == nullptr) {
    return;
  }
  CHECK(insert->table == "users");
  CHECK_EQ(insert->columns.size(), 3u);
  CHECK_EQ(insert->values.size(), 1u);
  CHECK_EQ(insert->values[0].size(), 3u);
  CHECK_EQ(insert->values[0][0].as_int(), 1);
  CHECK_EQ(insert->values[0][1].as_str(), std::string("Alice"));
  // NULL 与空串必须区分
  CHECK(insert->values[0][2].is_null());
}

TEST(Builder, BuildInsertWithoutColumnList) {
  auto outcome = build_sql("INSERT INTO users VALUES (1, 'Bob', 30);");
  REQUIRE_QUERY(outcome, query);
  const auto *insert = query.insert();
  CHECK(insert != nullptr);
  if (insert != nullptr) {
    CHECK(insert->columns.empty());
    CHECK(insert->use_default_columns());
    CHECK_EQ(insert->values[0].size(), 3u);
  }
}

TEST(Builder, BuildInsertWithBooleanAndNegativeLiterals) {
  auto outcome = build_sql("INSERT INTO t VALUES (TRUE, FALSE, -42);");
  REQUIRE_QUERY(outcome, query);
  const auto *insert = query.insert();
  CHECK(insert != nullptr);
  if (insert == nullptr) {
    return;
  }
  CHECK(insert->values[0][0].is_bool());
  CHECK_EQ(insert->values[0][0].as_bool(), true);
  CHECK_EQ(insert->values[0][1].as_bool(), false);
  CHECK_EQ(insert->values[0][2].as_int(), -42);
}

TEST(Builder, BuildUpdateQuery) {
  auto outcome =
      build_sql("UPDATE users SET name = 'Jane', age = 31 WHERE id = 1;");
  REQUIRE_QUERY(outcome, query);

  CHECK(query.is_update());
  const auto *update = query.update();
  CHECK(update != nullptr);
  if (update == nullptr) {
    return;
  }
  CHECK(update->table == "users");
  CHECK_EQ(update->assignments.size(), 2u);
  CHECK(update->assignments[0].column == "name");
  CHECK_EQ(update->assignments[0].value.as_str(), std::string("Jane"));
  CHECK(update->assignments[1].column == "age");
  CHECK_EQ(update->assignments[1].value.as_int(), 31);
  CHECK(update->where != nullptr);
}

TEST(Builder, BuildDeleteQuery) {
  auto outcome = build_sql("DELETE FROM users WHERE id IN (1, 2, 3);");
  REQUIRE_QUERY(outcome, query);

  CHECK(query.is_delete());
  const auto *del = query.delete_();
  CHECK(del != nullptr);
  if (del == nullptr) {
    return;
  }
  CHECK(del->table == "users");
  CHECK(del->where != nullptr);
  CHECK(del->where->is_in());
}

TEST(Builder, BuildConditionTree) {
  auto outcome = build_sql(
      "SELECT * FROM users WHERE (age > 18 OR age < 10) AND name != 'x';");
  REQUIRE_QUERY(outcome, query);
  const auto *select = query.select();
  CHECK(select != nullptr);
  if (select == nullptr) {
    return;
  }
  CHECK(select->where != nullptr);
  CHECK(select->where->is_and());
  if (select->where->is_and()) {
    CHECK(select->where->child_count() == 2);
    const sql::Condition *left = select->where->child_at(0);
    CHECK(left != nullptr);
    CHECK(left->is_or());
  }
}

TEST(Builder, BuildDdlQueries) {
  // USE
  auto use = build_sql("USE shop;");
  REQUIRE_QUERY(use, use_q);
  CHECK(use_q.is_use_database());
  CHECK(use_q.use_database()->database == "shop");

  // CREATE DATABASE / DROP DATABASE
  auto create_db = build_sql("CREATE DATABASE shop;");
  REQUIRE_QUERY(create_db, create_db_q);
  CHECK(create_db_q.is_create_database());
  CHECK(create_db_q.create_database()->database == "shop");

  auto drop_db = build_sql("DROP DATABASE shop;");
  REQUIRE_QUERY(drop_db, drop_db_q);
  CHECK(drop_db_q.is_drop_database());
  CHECK(drop_db_q.drop_database()->database == "shop");

  // DROP TABLE
  auto drop_table = build_sql("DROP TABLE users;");
  REQUIRE_QUERY(drop_table, drop_table_q);
  CHECK(drop_table_q.is_drop_table());
  CHECK(drop_table_q.drop_table()->table == "users");
}

TEST(Builder, BuildCreateTableKeepsTypesAndLength) {
  auto outcome =
      build_sql("CREATE TABLE users (id INT PRIMARY KEY, nickname VARCHAR(32), "
                "bio TEXT, created DATETIME);");
  REQUIRE_QUERY(outcome, query);

  CHECK(query.is_create_table());
  const auto *create = query.create_table();
  CHECK(create != nullptr);
  if (create == nullptr) {
    return;
  }
  CHECK(create->table == "users");
  CHECK_EQ(create->columns.size(), 4u);
  CHECK(create->columns[0].name == "id");
  CHECK(create->columns[0].type == sql::DataType::INT);
  CHECK(create->columns[0].primary_key);
  CHECK(!create->columns[0].nullable);

  CHECK(create->columns[1].type == sql::DataType::VARCHAR);
  CHECK_EQ(create->columns[1].length, 32u);
  CHECK(create->columns[2].type == sql::DataType::TEXT);
  CHECK(create->columns[3].type == sql::DataType::DATETIME);
}

TEST(Builder, BuilderDoesNotTakeAstOwnership) {
  parser::Parser parser;
  auto parsed = parser.parse("SELECT * FROM users;");
  CHECK(parsed.success);

  stmt::StatementBuilder builder;
  auto built = builder.build(parsed.ast.get());
  CHECK(built.has_value());
  // AST 仍归 ParseResult 所有：这里再访问一次不应该崩溃
  CHECK(parsed.ast.get() != nullptr);
  CHECK(parsed.ast->type == NODE_SELECT);
}

TEST(Builder, AcceptsConstAst) {
  // builder 只读 AST：接口是 const ASTNode*，调用方保留所有权
  parser::Parser parser;
  auto parsed = parser.parse("SELECT id FROM users WHERE age > 18;");
  CHECK(parsed.success);
  if (!parsed.success) {
    return;
  }
  const ASTNode *ast = parsed.ast.get();
  stmt::StatementBuilder builder;
  auto built = builder.build(ast);
  CHECK(built.has_value());
  if (built.has_value()) {
    CHECK(built->is_select());
  }
  // 传 const 指针不会改变 AST：再读一次仍然有效
  CHECK(ast->type == NODE_SELECT);
}
