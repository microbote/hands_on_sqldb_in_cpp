// tests/test_parser/test_explain.cpp
//
// `EXPLAIN [ANALYZE] <语句>`：语句前缀，产出 NODE_EXPLAIN 包住被解释的语句。
//
// 以前 EXPLAIN 是 session/CLI 的**文本前缀**（自己摘掉再解析，还要把错误
// 列号换算回原文）。进了语法层之后：`EXPLAIN BEGIN` / `EXPLAIN SELCT`
// 由语法层一次说清，位置、高亮、注释都走同一条路径。
#include "test_framework.h"

#include "parser/ast.h"
#include "parser/lex_tokens.h"
#include "parser/parser.h"

#include <cstdlib>
#include <string>
#include <vector>

extern "C" {
#include <parser.tab.h>
}

namespace {

parser::ASTNodePtr parse_ast(const std::string &sql,
                             parser::ParseError *error) {
  parser::Parser parser;
  auto result = parser.parse(sql);
  if (!result.success) {
    if (error != nullptr && result.error.has_value()) {
      *error = *result.error;
    }
    return parser::ASTNodePtr();
  }
  return std::move(result.ast);
}

std::vector<LexToken> collect(const std::string &sql) {
  LexToken *tokens = nullptr;
  const int count = lex_collect_tokens(sql.c_str(), &tokens);
  std::vector<LexToken> out;
  if (count > 0 && tokens != nullptr) {
    out.assign(tokens, tokens + count);
  }
  std::free(tokens);
  return out;
}

} // namespace

TEST(ExplainPrefix, WrapsTheStatementWithoutAnalyze) {
  parser::ParseError error;
  auto ast = parse_ast("EXPLAIN SELECT * FROM users;", &error);
  CHECK(ast != nullptr);
  if (ast == nullptr) {
    return;
  }
  CHECK(ast->type == NODE_EXPLAIN);
  CHECK_STREQ(node_type_to_string(ast->type), "EXPLAIN");

  const auto *explain = reinterpret_cast<const ExplainNode *>(ast->data);
  CHECK_EQ(explain->analyze, 0);
  CHECK(explain->statement != nullptr);
  if (explain->statement != nullptr) {
    // 内层还是原来的节点类型：前缀不影响被解释的语句
    CHECK(explain->statement->type == NODE_SELECT);
  }
  // 前缀算进整条语句的位置
  CHECK_EQ(ast->span.begin_column, 1u);
  CHECK_EQ(ast->span.end_column, 28u);

  char buffer[512];
  const int written =
      print_explain_node(ast.get(), 0, 0, buffer, sizeof(buffer));
  CHECK(written > 0);
  CHECK(std::string(buffer).find("EXPLAIN(analyze=0)") != std::string::npos);
}

TEST(ExplainPrefix, AnalyzeFlagIsCarriedOnTheNode) {
  parser::ParseError error;
  auto ast = parse_ast("EXPLAIN ANALYZE SELECT id FROM users;", &error);
  CHECK(ast != nullptr);
  if (ast == nullptr) {
    return;
  }
  const auto *explain = reinterpret_cast<const ExplainNode *>(ast->data);
  CHECK_EQ(explain->analyze, 1);
  CHECK(explain->statement != nullptr);
  if (explain->statement != nullptr) {
    CHECK(explain->statement->type == NODE_SELECT);
  }

  char buffer[512];
  print_explain_node(ast.get(), 0, 0, buffer, sizeof(buffer));
  CHECK(std::string(buffer).find("EXPLAIN(analyze=1)") != std::string::npos);
}

TEST(ExplainPrefix, WrapsEveryStatementKindIncludingTransaction) {
  // 写语句 / DDL / 事务控制都能被包住：合不合法由上层判断
  // （session：DDL 与 BEGIN 报 NOT_SUPPORTED），语法层只负责认出前缀
  struct Case {
    const char *sql;
    NodeType inner;
  };
  const Case cases[] = {
      {"EXPLAIN INSERT INTO users (id) VALUES (1);", NODE_INSERT},
      {"EXPLAIN UPDATE users SET age = 1;", NODE_UPDATE},
      {"EXPLAIN DELETE FROM users;", NODE_DELETE},
      {"EXPLAIN CREATE TABLE t (id INT PRIMARY KEY);", NODE_CREATE_TABLE},
      {"EXPLAIN BEGIN;", NODE_TRANSACTION},
      {"EXPLAIN COMMIT WORK;", NODE_TRANSACTION},
  };
  for (const Case &item : cases) {
    parser::ParseError error;
    auto ast = parse_ast(item.sql, &error);
    CHECK(ast != nullptr);
    if (ast == nullptr) {
      continue;
    }
    CHECK(ast->type == NODE_EXPLAIN);
    const auto *explain = reinterpret_cast<const ExplainNode *>(ast->data);
    CHECK(explain->statement != nullptr);
    if (explain->statement != nullptr) {
      CHECK(explain->statement->type == item.inner);
    }
  }
}

TEST(ExplainPrefix, KeywordsAreCaseInsensitiveAndComeFromTheLexer) {
  parser::ParseError error;
  auto ast = parse_ast("  explain   analyze   select * from users ;", &error);
  CHECK(ast != nullptr);
  if (ast != nullptr) {
    const auto *explain = reinterpret_cast<const ExplainNode *>(ast->data);
    CHECK_EQ(explain->analyze, 1);
    // 前缀空白算进语句起点
    CHECK_EQ(ast->span.begin_column, 3u);
  }

  const auto tokens = collect("EXPLAIN ANALYZE SELECT 1;");
  // EXPLAIN ANALYZE SELECT 1 ;  -> 5 个 token
  CHECK_EQ(tokens.size(), 5u);
  if (tokens.size() == 5u) {
    CHECK(tokens[0].kind == TOK_EXPLAIN);
    CHECK(tokens[1].kind == TOK_ANALYZE);
    CHECK(tokens[2].kind == TOK_SELECT);
  }
}

TEST(ExplainPrefix, IncompleteOrMisspelledFormsAreRejected) {
  // 只有前缀没有语句
  CHECK(parse_ast("EXPLAIN;", nullptr) == nullptr);
  CHECK(parse_ast("EXPLAIN ANALYZE;", nullptr) == nullptr);
  // ANALYZE 只是 EXPLAIN 的修饰词，单独出现不是语句
  CHECK(parse_ast("ANALYZE SELECT * FROM users;", nullptr) == nullptr);
  // 只认整词：EXPLAINX / ANALYZEd 是普通标识符
  CHECK(parse_ast("EXPLAINX SELECT * FROM users;", nullptr) == nullptr);
  CHECK(parse_ast("EXPLAIN ANALYZEd SELECT * FROM users;", nullptr) == nullptr);
}
