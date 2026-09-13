// tests/test_parser/test_transaction.cpp
//
// 事务控制语句：BEGIN / COMMIT / ROLLBACK 及其标准别名。
//
// 这些关键字原先在 session 层用"整条语句逐字符比对"的方式识别 ——
// 语法层完全不认识它们，于是注释、大小写、`EXPLAIN BEGIN` 之类的组合
// 都绕过了词法/语法检查。现在它们回到 sql.l / sql.y：
//   - 关键字只有一处定义（高亮/工具自动跟随）；
//   - 别名在语法动作里归一化成三种 kind；
//   - 不支持的形式（AND CHAIN / 保存点 / 事务隔离级别）给出明确原因，
//     而不是笼统的 "syntax error"。
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

// 解析成功时把 AST 交出去；失败返回 nullptr（并可通过 error 拿信息）
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

// 解析一条事务语句，检查根节点类型并返回 kind（失败返回 -1）
int transaction_kind(const std::string &sql) {
  parser::ParseError error;
  auto ast = parse_ast(sql, &error);
  if (ast == nullptr) {
    return -1;
  }
  if (ast->type != NODE_TRANSACTION) {
    return -2;
  }
  return static_cast<int>(
      reinterpret_cast<const TransactionNode *>(ast->data)->kind);
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

bool error_contains(const std::string &sql, const std::string &needle) {
  parser::ParseError error;
  auto ast = parse_ast(sql, &error);
  if (ast != nullptr) {
    return false;
  }
  return error.message.find(needle) != std::string::npos;
}

} // namespace

TEST(Transaction, BeginFormsAndAliases) {
  CHECK_EQ(transaction_kind("BEGIN;"), (int)TXN_BEGIN);
  CHECK_EQ(transaction_kind("BEGIN WORK;"), (int)TXN_BEGIN);
  CHECK_EQ(transaction_kind("BEGIN TRANSACTION;"), (int)TXN_BEGIN);
  CHECK_EQ(transaction_kind("START TRANSACTION;"), (int)TXN_BEGIN);

  // 关键字大小写不敏感（%option case-insensitive）
  CHECK_EQ(transaction_kind("begin;"), (int)TXN_BEGIN);
  CHECK_EQ(transaction_kind("Begin Work;"), (int)TXN_BEGIN);
  // 结尾分号是语法层的要求（session/CLI 负责补，见 normalize_sql）
  CHECK(parse_ast("START TRANSACTION", nullptr) == nullptr);
}

TEST(Transaction, CommitFormsAndAliases) {
  CHECK_EQ(transaction_kind("COMMIT;"), (int)TXN_COMMIT);
  CHECK_EQ(transaction_kind("COMMIT WORK;"), (int)TXN_COMMIT);
  CHECK_EQ(transaction_kind("END;"), (int)TXN_COMMIT);
  CHECK_EQ(transaction_kind("end work;"), (int)TXN_COMMIT);
}

TEST(Transaction, RollbackFormsAndAliases) {
  CHECK_EQ(transaction_kind("ROLLBACK;"), (int)TXN_ROLLBACK);
  CHECK_EQ(transaction_kind("ROLLBACK WORK;"), (int)TXN_ROLLBACK);
  CHECK_EQ(transaction_kind("ABORT;"), (int)TXN_ROLLBACK);
  CHECK_EQ(transaction_kind("abort work;"), (int)TXN_ROLLBACK);
}

TEST(Transaction, NodeNamePrintingAndSpan) {
  parser::ParseError error;
  auto ast = parse_ast("  COMMIT WORK ;", &error);
  CHECK(ast != nullptr);
  if (ast == nullptr) {
    return;
  }
  CHECK_STREQ(node_type_to_string(ast->type), "TRANSACTION");
  CHECK_STREQ(transaction_kind_to_string(TXN_COMMIT), "COMMIT");

  // 前缀空白算进语句起点，"WORK" 之后是第 14 列（不含分号）
  CHECK_EQ(ast->span.begin_column, 3u);
  CHECK_EQ(ast->span.end_column, 14u);

  char buffer[256];
  const int written =
      print_transaction_node(ast.get(), 0, 0, buffer, sizeof(buffer));
  CHECK(written > 0);
  CHECK(std::string(buffer).find("TRANSACTION(COMMIT)") != std::string::npos);
}

TEST(Transaction, KeywordsComeFromTheLexer) {
  const auto tokens = collect("START TRANSACTION;");
  CHECK_EQ(tokens.size(), 3u);
  if (tokens.size() != 3u) {
    return;
  }
  // 高亮器按 token kind 上色：这里确认事务关键字是 TOK_*（>= 258）
  // 而不是被当成标识符 —— 否则高亮会漏掉它们。
  CHECK(tokens[0].kind == TOK_START);
  CHECK(tokens[1].kind == TOK_TRANSACTION);
  CHECK(tokens[2].kind == ';');
}

TEST(Transaction, UnsupportedFormsSayWhy) {
  CHECK(
      error_contains("BEGIN DEFERRED;", "transaction modes are not supported"));
  CHECK(error_contains("START TRANSACTION ISOLATION LEVEL SERIALIZABLE;",
                       "transaction modes are not supported"));
  CHECK(error_contains("COMMIT AND CHAIN;",
                       "COMMIT AND CHAIN/NO CHAIN is not supported"));
  CHECK(error_contains("ROLLBACK AND NO CHAIN;",
                       "ROLLBACK AND CHAIN/NO CHAIN is not supported"));
  CHECK(error_contains("ROLLBACK TO SAVEPOINT sp1;",
                       "ROLLBACK TO SAVEPOINT is not supported"));
}

TEST(Transaction, LooksLikeKeywordButIsNot) {
  // 只认整词：BEGINNER / COMMITTED 仍然是标识符，出现在这里必然语法错
  CHECK(parse_ast("BEGINNER;", nullptr) == nullptr);
  CHECK(parse_ast("COMMITTED;", nullptr) == nullptr);
  // 事务语句是独立语句，后面不能再跟别的东西
  CHECK(parse_ast("BEGIN SELECT * FROM t;", nullptr) == nullptr);
  CHECK(parse_ast("COMMIT SELECT * FROM t;", nullptr) == nullptr);
  // 事务关键字不再能当标识符（与 END/DESC 等一样被保留）
  CHECK(parse_ast("SELECT commit FROM t;", nullptr) == nullptr);
}
