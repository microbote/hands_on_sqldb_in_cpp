// tests/test_parser/test_source_span.cpp
//
// 位置跟踪：sql.l 维护行列，sql.y 用 Bison 的 @$/@n 把位置写进 AST 节点。
// 这些位置后面会随 StmtError 一起返回，供 CLI 画 caret。
#include "test_framework.h"

#include "parser/parser.h"

#include <string>

namespace {

// SELECT * FROM users WHERE age > 18;
//                                  ^位置参考
const char *kSelect = "SELECT * FROM users WHERE age > 18;";

} // namespace

TEST(SourceSpan, StatementSpanCoversWholeStatement) {
  parser::Parser parser;
  auto result = parser.parse(kSelect);
  CHECK(result.success);
  if (!result.success) {
    return;
  }
  CHECK(sspan_valid(result.ast->span));
  CHECK_EQ(result.ast->span.begin_line, 1u);
  CHECK_EQ(result.ast->span.begin_column, 1u);
  // "SELECT * FROM users WHERE age > 18" -> 结束列 35（不含 ';'）
  CHECK_EQ(result.ast->span.end_column, 35u);
}

TEST(SourceSpan, TableNameHasItsOwnSpan) {
  parser::Parser parser;
  auto result = parser.parse(kSelect);
  CHECK(result.success);
  if (!result.success) {
    return;
  }
  const auto *select = (const SelectNode *)result.ast->data;
  // "users" 在第 15 列（1-based），[15, 20)
  CHECK_EQ(select->table_span.begin_column, 15u);
  CHECK_EQ(select->table_span.end_column, 20u);
  CHECK_EQ(sspan_width(select->table_span), 5u);
}

TEST(SourceSpan, ConditionAndValueHavePreciseSpans) {
  parser::Parser parser;
  auto result = parser.parse(kSelect);
  CHECK(result.success);
  if (!result.success) {
    return;
  }
  const auto *select = (const SelectNode *)result.ast->data;
  CHECK(select->condition != nullptr);
  if (select->condition == nullptr) {
    return;
  }
  // 条件节点覆盖 "age > 18"（第 27 列起）
  CHECK_EQ(select->condition->span.begin_column, 27u);

  const auto *compare = (const CompareNode *)select->condition->data;
  CHECK(compare->right != nullptr);
  if (compare->right != nullptr) {
    // 字面量 "18" 在第 33 列，[33, 35)
    CHECK_EQ(compare->right->span.begin_column, 33u);
    CHECK_EQ(compare->right->span.end_column, 35u);
  }
}

TEST(SourceSpan, ColumnListItemsHaveSpans) {
  parser::Parser parser;
  auto result = parser.parse("SELECT id, name FROM users;");
  CHECK(result.success);
  if (!result.success) {
    return;
  }
  const auto *select = (const SelectNode *)result.ast->data;
  const auto *list = (const ASTNodeList *)select->columns->data;
  const ASTNode *id = list->head;
  const ASTNode *name = id->next;
  CHECK(id != nullptr && name != nullptr);
  if (id == nullptr || name == nullptr) {
    return;
  }
  CHECK_EQ(id->span.begin_column, 8u); // "id"
  CHECK_EQ(id->span.end_column, 10u);
  CHECK_EQ(name->span.begin_column, 12u); // "name"
  CHECK_EQ(name->span.end_column, 16u);
}

TEST(SourceSpan, LineAndColumnAdvanceAcrossLines) {
  parser::Parser parser;
  auto result = parser.parse("SELECT id,\n       name FROM users;");
  CHECK(result.success);
  if (!result.success) {
    return;
  }
  const auto *select = (const SelectNode *)result.ast->data;
  const auto *list = (const ASTNodeList *)select->columns->data;
  const ASTNode *name = list->head->next;
  CHECK(name != nullptr);
  if (name == nullptr) {
    return;
  }
  CHECK_EQ(name->span.begin_line, 2u);   // 第二行
  CHECK_EQ(name->span.begin_column, 8u); // 行内第 8 列
  CHECK_EQ(name->span.end_column, 12u);
}

TEST(SourceSpan, BlankLinesAndLeadingNewlinesAreCounted) {
  parser::Parser parser;
  auto result = parser.parse("\n\nSELECT * FROM users;");
  CHECK(result.success);
  if (!result.success) {
    return;
  }
  CHECK_EQ(result.ast->span.begin_line, 3u);
  CHECK_EQ(result.ast->span.begin_column, 1u);
  const auto *select = (const SelectNode *)result.ast->data;
  CHECK_EQ(select->table_span.begin_line, 3u);
  CHECK_EQ(select->table_span.begin_column, 15u);
}

TEST(SourceSpan, ColumnDefinitionHasSpan) {
  parser::Parser parser;
  auto result = parser.parse(
      "CREATE TABLE users (id INT PRIMARY KEY, name VARCHAR(32));");
  CHECK(result.success);
  if (!result.success) {
    return;
  }
  const auto *create = (const CreateTableNode *)result.ast->data;
  // 表名 "users" 在第 14 列
  CHECK_EQ(create->table_span.begin_column, 14u);
  CHECK_EQ(create->table_span.end_column, 19u);

  const auto *columns = (const ASTNodeList *)create->columns->data;
  const ASTNode *first = columns->head;
  const ASTNode *second = first->next;
  CHECK(first != nullptr && second != nullptr);
  if (first == nullptr || second == nullptr) {
    return;
  }
  // "id INT PRIMARY KEY" 从第 21 列开始
  CHECK_EQ(first->span.begin_column, 21u);
  // "name VARCHAR(32)" 从第 41 列开始
  CHECK_EQ(second->span.begin_column, 41u);
}

TEST(SourceSpan, ErrorPositionUsesLineNumber) {
  // 语法错误的位置现在是结构化的 (line, column)，message 保持纯文本
  parser::Parser parser;
  auto result = parser.parse("\n\n\nSELECT * FROM WHERE;");
  CHECK(!result.success);
  CHECK(result.error.has_value());
  if (result.error.has_value()) {
    CHECK_EQ(result.error->line, 4);
    CHECK(result.error->column > 0);
    CHECK(result.error->to_string().find("line 4") != std::string::npos);
  }
}
