// tests/test_statement/test_source_span.cpp
//
// StmtError 的位置信息：builder/validator 的报错能指回 SQL 文本的具体位置，
// 并可以用 sspan_caret() 画出来。
#include "test_framework.h"

#include "memory_catalog.h"
#include "parser/parser.h"
#include "statement/source_span.h"
#include "statement/stmt_builder.h"
#include "statement/stmt_validator.h"

#include <string>

namespace {

MemoryCatalog make_catalog() {
  MemoryCatalog catalog;
  catalog.set_open(true);
  catalog.create_database(sql::Identifier("shop"));
  catalog.use_database(sql::Identifier("shop"));
  catalog.create_table(sql::Identifier("shop"), make_users_schema());
  return catalog;
}

// 解析 + 构建 + 校验，返回诊断信息（错误值 + AST 的位置表 + SQL 原文）
struct Diagnostic {
  stmt::StmtError error;
  std::string sql;
  std::vector<stmt::NamedSpan> spans;
  bool ok() const { return error.ok(); }
};

Diagnostic run_with_spans(const sql::Catalog &catalog, const std::string &sql) {
  Diagnostic diag;
  diag.sql = sql;
  parser::Parser parser;
  auto parsed = parser.parse(sql);
  if (!parsed.success) {
    diag.error =
        stmt::StmtError(stmt::StmtErrorCode::AST_IS_NULL, "parse failed");
    return diag;
  }
  diag.spans = stmt::collect_source_spans(parsed.ast.get());

  stmt::StatementBuilder builder;
  auto query = builder.build(parsed.ast.get());
  if (!query.has_value()) {
    diag.error = query.error();
    return diag;
  }
  // validator 拿到 resolver 后，报错会带上对应名字的位置
  stmt::StatementValidator validator(
      catalog, stmt::make_span_resolver(parsed.ast.get()));
  auto status = validator.validate(*query);
  if (!status.has_value()) {
    diag.error = status.error();
  }
  return diag;
}

} // namespace

TEST(SourceSpan, ValidatorReportsPositionOfMissingTable) {
  auto catalog = make_catalog();
  const std::string sql = "SELECT * FROM nosuchtable;";
  const auto diag = run_with_spans(catalog, sql);

  CHECK(diag.error.code == stmt::StmtErrorCode::TABLE_NOT_FOUND);
  CHECK(sspan_valid(diag.error.span));
  // "nosuchtable" 从第 15 列开始、长 11
  CHECK_EQ(diag.error.span.begin_line, 1u);
  CHECK_EQ(diag.error.span.begin_column, 15u);
  CHECK_EQ(diag.error.span.end_column, 26u);
  CHECK(diag.error.to_string().find("line 1:15") != std::string::npos);
}

TEST(SourceSpan, ValidatorReportsPositionOfMissingColumn) {
  auto catalog = make_catalog();
  const auto diag = run_with_spans(catalog, "SELECT badcol FROM users;");

  CHECK(diag.error.code == stmt::StmtErrorCode::COLUMN_NOT_FOUND);
  CHECK(sspan_valid(diag.error.span));
  CHECK_EQ(diag.error.span.begin_column, 8u); // "badcol"
  CHECK_EQ(diag.error.span.end_column, 14u);
}

TEST(SourceSpan, WhereClauseColumnPosition) {
  auto catalog = make_catalog();
  const auto diag =
      run_with_spans(catalog, "SELECT * FROM users WHERE nope = 1;");

  CHECK(diag.error.code == stmt::StmtErrorCode::COLUMN_NOT_FOUND);
  CHECK(sspan_valid(diag.error.span));
  CHECK_EQ(diag.error.span.begin_column, 27u); // "nope"
}

TEST(SourceSpan, SpansAreAbsentWithoutResolver) {
  // 不提供 resolver 时，错误仍然可用，只是没有位置
  MemoryCatalog catalog;
  catalog.set_open(true);
  catalog.create_database(sql::Identifier("shop"));
  catalog.use_database(sql::Identifier("shop"));

  parser::Parser parser;
  auto parsed = parser.parse("SELECT * FROM users;");
  CHECK(parsed.success);
  stmt::StatementBuilder builder;
  auto query = builder.build(parsed.ast.get());
  CHECK(query.has_value());
  stmt::StatementValidator validator(catalog); // 无 resolver
  auto status = validator.validate(*query);
  CHECK(!status.has_value());
  CHECK(!sspan_valid(status.error().span));
}

TEST(SourceSpan, BuilderErrorKeepsStatementPosition) {
  // 手工构造一个"缺表名"的 SELECT：位置来自节点的 span（builder 兜底）
  ASTNode *select =
      make_select_node(nullptr, nullptr, nullptr, nullptr, nullptr);
  ast_set_span(select, 2, 5, 2, 40);

  stmt::StatementBuilder builder;
  auto result = builder.build(select);
  CHECK(!result.has_value());
  CHECK(result.error().code == stmt::StmtErrorCode::EMPTY_STATEMENT);
  CHECK(sspan_valid(result.error().span));
  CHECK_EQ(result.error().span.begin_line, 2u);
  CHECK_EQ(result.error().span.begin_column, 5u);
  free_ast(select);
}

TEST(SourceSpan, CaretRenderingPointsAtTheColumn) {
  const std::string sql = "SELECT * FROM users WHERE nope = 1;";
  const SSpan span = sspan_make(1, 27, 1, 31); // "nope"

  const std::string caret = stmt::sspan_caret(sql, span, "column not found");
  CHECK(caret.find(sql) != std::string::npos);
  // caret 行：两个空格缩进 + 26 个空格 + 4 个 ^
  const std::string caret_line =
      "\n  " + std::string(26, ' ') + std::string(4, '^');
  CHECK(caret.find(caret_line) != std::string::npos);
  CHECK(caret.find("column not found") != std::string::npos);
  CHECK(caret.find("line 1:27-31") != std::string::npos);
}

TEST(SourceSpan, SpanToStringFormatting) {
  CHECK_EQ(stmt::sspan_to_string(sspan_make(1, 8, 1, 10)),
           std::string("line 1:8-10"));
  CHECK_EQ(stmt::sspan_to_string(sspan_make(2, 3, 4, 5)),
           std::string("line 2:3..4:5"));
  CHECK(stmt::sspan_to_string(sspan_unknown()).empty());

  // 未知位置不画 caret
  CHECK(stmt::sspan_caret("SELECT 1;", sspan_unknown()).empty());
}

TEST(SourceSpan, CollectSourceSpansCoversTablesAndColumns) {
  parser::Parser parser;
  auto parsed = parser.parse("SELECT id, name FROM users WHERE age > 18;");
  CHECK(parsed.success);
  const auto spans = stmt::collect_source_spans(parsed.ast.get());

  // users / id / name / age 都应该在表里
  auto find = [&spans](const char *name) {
    for (const auto &entry : spans) {
      if (entry.name == name) {
        return entry.span;
      }
    }
    return sspan_unknown();
  };
  CHECK(sspan_valid(find("users")));
  CHECK(sspan_valid(find("id")));
  CHECK(sspan_valid(find("name")));
  CHECK(sspan_valid(find("age")));
  CHECK(!sspan_valid(find("nosuchname")));
}

TEST(SourceSpan, InsertValueSpanPointsAtTheLiteral) {
  auto catalog = make_catalog();
  // 'abc' 从第 35 列开始（[35, 40)），类型与 INT 列不匹配
  const std::string sql = "INSERT INTO users (id, age) VALUES (1, 'abc');";
  const auto diag = run_with_spans(catalog, sql);

  CHECK(diag.error.code == stmt::StmtErrorCode::COLUMN_TYPE_MISMATCH);
  CHECK(sspan_valid(diag.error.span));
  CHECK_EQ(diag.error.span.begin_column, 40u);
  CHECK_EQ(diag.error.span.end_column, 45u);

  // caret 正好落在字面量上
  const std::string caret =
      stmt::sspan_caret(sql, diag.error.span, diag.error.message);
  CHECK(caret.find("\n  " + std::string(39, ' ') + std::string(5, '^')) !=
        std::string::npos);
}

TEST(SourceSpan, InsertValueSpansArePerColumn) {
  auto catalog = make_catalog();
  // 第二个值是超长字符串：位置应该指向它，而不是第一个值
  const std::string too_long(33, 'x');
  const std::string sql =
      "INSERT INTO users (id, name) VALUES (1, '" + too_long + "');";
  const auto diag = run_with_spans(catalog, sql);

  CHECK(diag.error.code == stmt::StmtErrorCode::VALUE_OUT_OF_RANGE);
  CHECK(sspan_valid(diag.error.span));
  CHECK_EQ(diag.error.span.begin_column, 41u); // 第二个值（字符串字面量）
  CHECK_EQ(sspan_width(diag.error.span), 35u); // 33 个字符 + 两个引号
}

TEST(SourceSpan, UpdateValueSpanPointsAtTheLiteral) {
  auto catalog = make_catalog();
  const std::string sql = "UPDATE users SET age = 'abc' WHERE id = 1;";
  const auto diag = run_with_spans(catalog, sql);

  CHECK(diag.error.code == stmt::StmtErrorCode::COLUMN_TYPE_MISMATCH);
  CHECK(sspan_valid(diag.error.span));
  // 'abc' 从第 24 列开始
  CHECK_EQ(diag.error.span.begin_column, 24u);
  CHECK_EQ(diag.error.span.end_column, 29u);
}

TEST(SourceSpan, ValueSpanFallsBackToColumnWhenMissing) {
  // 手工构造的 Query 没有值位置：退回列名位置（resolver 提供）
  MemoryCatalog catalog;
  catalog.set_open(true);
  catalog.create_database(sql::Identifier("shop"));
  catalog.use_database(sql::Identifier("shop"));
  catalog.create_table(sql::Identifier("shop"), make_users_schema());

  parser::Parser parser;
  auto parsed = parser.parse("UPDATE users SET age = 1;");
  CHECK(parsed.success);
  stmt::StatementBuilder builder;
  auto query = builder.build(parsed.ast.get());
  CHECK(query.has_value());
  if (!query.has_value()) {
    return;
  }
  // 人为把值位置抹掉，模拟"没有位置信息"的 Query
  auto *update = query->update();
  CHECK(update != nullptr);
  CHECK_EQ(update->assignments.size(), 1u);
  update->assignments[0].value_span = sspan_unknown();
  update->assignments[0].value = sql::Value(std::string("abc")); // 类型不匹配

  stmt::StatementValidator validator(
      catalog, stmt::make_span_resolver(parsed.ast.get()));
  auto status = validator.validate(*query);
  CHECK(!status.has_value());
  CHECK(status.error().code == stmt::StmtErrorCode::COLUMN_TYPE_MISMATCH);
  CHECK(sspan_valid(status.error().span));
  CHECK_EQ(status.error().span.begin_column, 18u); // SET 里的 "age"
}
