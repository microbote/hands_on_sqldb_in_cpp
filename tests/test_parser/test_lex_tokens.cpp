// tests/test_parser/test_lex_tokens.cpp
//
// 词法 token 流导出（lex_collect_tokens）：高亮等工具与解析共用同一套规则。
#include "test_framework.h"

#include "parser/lex_tokens.h"

extern "C" {
#include <parser.tab.h>
}

#include <cctype>
#include <cstdlib>
#include <string>
#include <vector>

namespace {

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

TEST(LexTokens, KindsOffsetsAndSpans) {
  const std::string sql = "SELECT id FROM t WHERE a >= 5;";
  const auto tokens = collect(sql);

  // SELECT id FROM t WHERE a >= 5 ;  -> 9 个 token
  CHECK_EQ(tokens.size(), 9u);
  if (tokens.size() != 9) {
    return;
  }
  CHECK(tokens[0].kind == TOK_SELECT);
  CHECK(tokens[1].kind == TOK_IDENT);
  CHECK(tokens[2].kind == TOK_FROM);
  CHECK(tokens[3].kind == TOK_IDENT);
  CHECK(tokens[4].kind == TOK_WHERE);
  CHECK(tokens[5].kind == TOK_IDENT);
  CHECK(tokens[6].kind == TOK_GE);
  CHECK(tokens[7].kind == TOK_NUMBER);
  CHECK(tokens[8].kind == ';');

  // offset/length 能切回原文
  for (const auto &token : tokens) {
    CHECK(sql.substr(token.offset, token.length) ==
          sql.substr(token.offset, token.length));
    CHECK(sspan_valid(token.span));
  }
  CHECK_EQ(sql.substr(tokens[0].offset, tokens[0].length),
           std::string("SELECT"));
  CHECK_EQ(sql.substr(tokens[7].offset, tokens[7].length), std::string("5"));
  // 位置：SELECT 在第 1 列，5 在第 29 列
  CHECK_EQ(tokens[0].span.begin_column, 1u);
  CHECK_EQ(tokens[0].span.end_column, 7u);
  CHECK_EQ(tokens[7].span.begin_column, 29u);
}

TEST(LexTokens, SkipsWhitespaceButKeepsOffsetsConsistent) {
  // 空白不产生 token，但 offset 反映原文位置
  const std::string sql = "  SELECT   id\nFROM t;";
  const auto tokens = collect(sql);
  CHECK_EQ(tokens.size(), 5u); // SELECT id FROM t ;
  if (tokens.size() != 5) {
    return;
  }
  CHECK(tokens[0].kind == TOK_SELECT);
  CHECK_EQ(tokens[0].offset, 2u); // 前导两个空格
  CHECK(tokens[0].span.begin_column == 3u);
  CHECK(tokens[1].kind == TOK_IDENT);
  CHECK_EQ(sql.substr(tokens[1].offset, tokens[1].length), std::string("id"));
  CHECK_EQ(tokens[1].span.begin_line, 1u);
  // FROM / t / ';' 都在第二行
  CHECK_EQ(tokens[2].span.begin_line, 2u);
  CHECK_EQ(tokens[3].span.begin_line, 2u);
}

TEST(LexTokens, EmptyInputYieldsNoTokens) {
  LexToken *tokens = nullptr;
  const int count = lex_collect_tokens("", &tokens);
  CHECK_EQ(count, 0);
  CHECK(tokens == nullptr);
  std::free(tokens);
}

TEST(LexTokens, StringAndNumberTokensKeepTheirText) {
  const std::string sql = "INSERT INTO t VALUES ('it''s', -42, NULL);";
  const auto tokens = collect(sql);
  CHECK(!tokens.empty());
  bool saw_string = false;
  bool saw_number = false;
  bool saw_null_literal = false;
  for (const auto &token : tokens) {
    if (token.kind == TOK_STRING) {
      saw_string = true;
      CHECK_EQ(sql.substr(token.offset, token.length),
               std::string("'it''s'")); // 原样（含转义）
    } else if (token.kind == TOK_NUMBER) {
      saw_number = true;
      CHECK_EQ(sql.substr(token.offset, token.length), std::string("42"));
    } else if (token.kind == TOK_NULL) {
      saw_null_literal = true;
    }
  }
  CHECK(saw_string);
  CHECK(saw_number);
  CHECK(saw_null_literal);
}

TEST(LexTokens, UnknownCharacterIsASingleCharToken) {
  // 未识别的字符由兜底规则返回字符码（与解析路径同一行为）
  const std::string sql = "SELECT # FROM t;";
  const auto tokens = collect(sql);
  CHECK(!tokens.empty());
  bool saw_hash = false;
  for (const auto &token : tokens) {
    if (token.kind == '#') {
      saw_hash = true;
      CHECK_EQ(token.length, 1u);
    }
  }
  CHECK(saw_hash);
}

TEST(LexTokens, TypesAreReportedAsTypeName) {
  const std::string sql = "CREATE TABLE t (a VARCHAR(32) PRIMARY KEY);";
  const auto tokens = collect(sql);
  bool saw_type = false;
  for (const auto &token : tokens) {
    if (token.kind == TOK_TYPE_NAME) {
      saw_type = true;
      CHECK_EQ(sql.substr(token.offset, token.length), std::string("VARCHAR"));
    }
  }
  CHECK(saw_type);
}

// ---------------------------------------------------------------
// 关键字清单（lex_keywords）：客户端的 TAB 补全数据源。
//
// sql.l 里"关键字规则"和"导出清单"是同一文件里的两张表，这条用例保证它们
// **不会漂移**：清单里的每个词都必须被词法层当成关键字（不是普通标识符），
// 而且大小写两种拼写落到同一个 token。
// ---------------------------------------------------------------
namespace {

// 把单个词喂进词法层，返回它被识别成的 token kind（空输入返回 0）
int lex_single_kind(const std::string &word) {
  LexToken *tokens = nullptr;
  const int count = lex_collect_tokens(word.c_str(), &tokens);
  int kind = 0;
  if (count >= 1 && tokens != nullptr) {
    kind = tokens[0].kind;
  }
  std::free(tokens);
  return kind;
}

std::string upper_copy(const std::string &text) {
  std::string out = text;
  for (char &c : out) {
    c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
  }
  return out;
}

} // namespace

TEST(LexTokens, KeywordListMatchesTheLexerAndIsCaseInsensitive) {
  const char *const *keywords = nullptr;
  const int count = lex_keywords(&keywords);
  CHECK(keywords != nullptr);
  CHECK(count > 0);
  if (keywords == nullptr || count <= 0) {
    return;
  }

  std::vector<std::string> seen;
  for (int i = 0; i < count; ++i) {
    const std::string word = keywords[i];
    CHECK(!word.empty());

    // 清单本身必须是小写（补全提示、文档都用小写形态）
    for (const char c : word) {
      CHECK_FALSE(std::isupper(static_cast<unsigned char>(c)) != 0);
    }
    // 不重复
    for (const std::string &other : seen) {
      CHECK_NE(word, other);
    }
    seen.push_back(word);

    // 每个词都必须是"关键字"，不能退化成标识符（TOK_IDENT 说明规则漏了）
    const int kind = lex_single_kind(word);
    CHECK(kind != TOK_IDENT);
    CHECK(kind != TOK_TYPE_NAME);
    CHECK(kind != 0);

    // 大小写无关：全大写拼写必须落到同一个 token
    const int upper_kind = lex_single_kind(upper_copy(word));
    CHECK_EQ(upper_kind, kind);
  }
}

TEST(LexTokens, KeywordListCoversCoreSqlVocabulary) {
  // 正向清单（每个词都必须出现）——防止有人删规则时顺手删清单里的词。
  const std::vector<std::string> required = {
      "select", "from",     "where",   "insert",  "into",   "values",
      "update", "set",      "delete",  "create",  "drop",   "database",
      "table",  "order",    "by",      "limit",   "offset", "begin",
      "commit", "rollback", "explain", "analyze", "null",   "in",
      "is",     "like",     "and",     "or",      "not",
  };
  const char *const *keywords = nullptr;
  const int count = lex_keywords(&keywords);
  CHECK(keywords != nullptr);
  if (keywords == nullptr) {
    return;
  }
  for (const std::string &word : required) {
    bool found = false;
    for (int i = 0; i < count; ++i) {
      if (word == keywords[i]) {
        found = true;
      }
    }
    CHECK(found);
  }
}
