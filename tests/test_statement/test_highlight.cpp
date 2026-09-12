// tests/test_statement/test_highlight.cpp
//
// 语法高亮：colors=false 时原样输出；colors=true 时按类别上色。
#include "test_framework.h"

#include "common/ansi_color.h"
#include "statement/source_span.h"
#include "statement/sql_highlight.h"

#include <string>

namespace {

bool has_escape(const std::string &text) {
  return text.find("\x1b[") != std::string::npos;
}

} // namespace

TEST(Highlight, DisabledColorsReturnInputUnchanged) {
  const std::string sql = "SELECT id FROM users WHERE age > 18;";
  CHECK_EQ(stmt::highlight_sql(sql, false), sql);
  CHECK(!has_escape(stmt::highlight_sql(sql, false)));
}

TEST(Highlight, EnabledColorsWrapKeywordsTypesStringsAndNumbers) {
  const std::string sql =
      "INSERT INTO users (id, name, created) VALUES (1, 'Alice', NULL);";
  const std::string colored = stmt::highlight_sql(sql, true);

  CHECK(has_escape(colored));
  // 关键字（粗体蓝）
  CHECK(colored.find(std::string(sql::ansi::kKeyword) + "INSERT") !=
        std::string::npos);
  // 类型名（青色）：这里用 CREATE TABLE 更直观
  const std::string ddl =
      "CREATE TABLE t (id BIGINT PRIMARY KEY, name VARCHAR);";
  const std::string colored_ddl = stmt::highlight_sql(ddl, true);
  CHECK(colored_ddl.find(std::string(sql::ansi::kCyan) + "BIGINT") !=
        std::string::npos);
  CHECK(colored_ddl.find(std::string(sql::ansi::kCyan) + "VARCHAR") !=
        std::string::npos);
  // 字符串（绿色）与数字（黄色）
  CHECK(colored.find(std::string(sql::ansi::kGreen) + "'Alice'") !=
        std::string::npos);
  CHECK(colored.find(std::string(sql::ansi::kYellow) + "1") !=
        std::string::npos);
}

TEST(Highlight, StrippingEscapesRecoversTheOriginalText) {
  const std::string sql =
      "SELECT a, b FROM t WHERE a = 'x' AND b > 10 ORDER BY a DESC LIMIT 3;";
  const std::string colored = stmt::highlight_sql(sql, true);

  // 去掉所有 ANSI 转义后应当与原文完全一致
  std::string plain;
  for (size_t i = 0; i < colored.size(); ++i) {
    if (colored[i] == '\x1b' && i + 1 < colored.size() &&
        colored[i + 1] == '[') {
      i += 2;
      while (i < colored.size() && colored[i] != 'm') {
        ++i;
      }
      continue; // 跳过 'm'
    }
    plain.push_back(colored[i]);
  }
  CHECK_EQ(plain, sql);
}

TEST(Highlight, ErrorSpanIsHighlighted) {
  const std::string sql = "SELECT * FROM users WHERE nope = 1;";
  const SSpan span = sspan_make(1, 27, 1, 31); // "nope"

  const std::string colored = stmt::highlight_span(sql, span, true);
  CHECK(colored.find(std::string(sql::ansi::kErrorSpan) + "nope" +
                     std::string(sql::ansi::kReset)) != std::string::npos);
  // 其它部分保持原样
  CHECK(colored.find("SELECT * FROM users WHERE ") != std::string::npos);

  // 关闭颜色时原样返回
  CHECK_EQ(stmt::highlight_span(sql, span, false), sql);
  // 未知位置原样返回
  CHECK_EQ(stmt::highlight_span(sql, sspan_unknown(), true), sql);
}

TEST(Highlight, CaretUsesColorsWhenAsked) {
  const std::string sql = "SELECT * FROM users WHERE nope = 1;";
  const SSpan span = sspan_make(1, 27, 1, 31);

  const std::string plain = stmt::sspan_caret(sql, span, "column not found");
  CHECK(!has_escape(plain));
  CHECK(plain.find("^^^^") != std::string::npos);

  const std::string colored =
      stmt::sspan_caret(sql, span, "column not found", true);
  CHECK(has_escape(colored));
  CHECK(colored.find(std::string(sql::ansi::kErrorSpan) + "^^^^") !=
        std::string::npos);
  // 出错片段本身也被高亮
  CHECK(colored.find(std::string(sql::ansi::kErrorSpan) + "nope") !=
        std::string::npos);
}

TEST(Highlight, ColorPolicyHelpers) {
  // 显式传 false 一定不上色；NO_COLOR 会让默认策略返回 false
  CHECK(!has_escape(stmt::highlight_sql("SELECT 1", false)));
  setenv("NO_COLOR", "1", 1);
  CHECK(!sql::ansi::enabled_by_default());
  unsetenv("NO_COLOR");
}

// ============================================================
// 高亮复用 lexer 的 token 流：不再有"关键字表"可漂移
// ============================================================

TEST(Highlight, KeywordsComeFromTheLexerSoAliasesAreColored) {
  // 回归：ASCENDING/DESCENDING 由 sql.l 认作关键字（TOK_ASC/TOK_DESC），
  // 旧的手写关键字表漏了它们；现在按 token kind 上色，自动跟随。
  const std::string sql = "SELECT * FROM t ORDER BY a ASCENDING, b DESCENDING;";
  const std::string colored = stmt::highlight_sql(sql, true);
  CHECK(colored.find(std::string(sql::ansi::kKeyword) + "ASCENDING") !=
        std::string::npos);
  CHECK(colored.find(std::string(sql::ansi::kKeyword) + "DESCENDING") !=
        std::string::npos);
  CHECK(colored.find(std::string(sql::ansi::kKeyword) + "ORDER") !=
        std::string::npos);
}

TEST(Highlight, CommentLikeTextFollowsLexerNotIntuition) {
  // lexer 目前**没有**注释规则，所以 "--" 会被拆成两个 '-' 字符 token，
  // 高亮器不会把它当注释（与解析器行为一致：这段其实是语法错误）。
  const std::string sql = "SELECT 1; -- not a comment yet";
  const std::string colored = stmt::highlight_sql(sql, true);
  CHECK(colored.find(std::string(sql::ansi::kDim)) == std::string::npos);
  // 但原文必须逐字节保留
  CHECK(!has_escape(sql));
}

TEST(Highlight, TrickyInputsRoundTripByteExact) {
  const std::string cases[] = {
      "SELECT 'it''s' FROM t;",
      "SELECT * FROM t WHERE a <> 1;",
      "SELECT \"quoted\";",
      "SELECT 1;\n-- trailing\n",
      "SELECT 'multi\nline' FROM t;",
      "SELECT # FROM t;",
      "   ",
      "",
  };
  for (const auto &sql : cases) {
    const std::string colored = stmt::highlight_sql(sql, true);
    // 去掉 ANSI 转义后必须与原文完全一致（token 之间的空隙原样复制）
    std::string plain;
    for (size_t i = 0; i < colored.size(); ++i) {
      if (colored[i] == '\x1b' && i + 1 < colored.size() &&
          colored[i + 1] == '[') {
        i += 2;
        while (i < colored.size() && colored[i] != 'm') {
          ++i;
        }
        continue;
      }
      plain.push_back(colored[i]);
    }
    CHECK_EQ(plain, sql);
  }
}
