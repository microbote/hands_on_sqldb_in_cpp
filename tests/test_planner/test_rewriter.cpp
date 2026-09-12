// tests/test_planner/test_rewriter.cpp
//
// QueryRewriter：结构化简（NOT 下推、扁平化、去重、吸收律、OR-of-EQ -> IN）。
// 断言用重写后条件的 to_string()，输入都走真实 parser。
#include "test_framework.h"

#include <string>

#include "planner/rewriter.h"
#include "planner_test_util.h"

namespace {

using plantest::build_query;

// 重写 SELECT 的 WHERE，返回条件的文本形式（无条件时返回 "-"）
std::string rewrite_where(const std::string &sql) {
  auto query = build_query(sql);
  if (!query.has_value()) {
    return "<parse-failed>";
  }
  plan::QueryRewriter rewriter;
  auto rewritten = rewriter.rewrite(*query);
  if (!rewritten.has_value()) {
    return "<rewrite-failed>";
  }
  const sql::SelectQuery *select = rewritten->select();
  if (select == nullptr || !select->where) {
    return "-";
  }
  return select->where->to_string();
}

} // namespace

TEST(Rewriter, PushdownNotFlipsCompare) {
  CHECK_EQ(rewrite_where("SELECT * FROM users WHERE NOT (age > 5)"),
           std::string("age <= 5"));
  CHECK_EQ(rewrite_where("SELECT * FROM users WHERE NOT (age >= 5)"),
           std::string("age < 5"));
  CHECK_EQ(rewrite_where("SELECT * FROM users WHERE NOT (age <= 5)"),
           std::string("age > 5"));
  CHECK_EQ(rewrite_where("SELECT * FROM users WHERE NOT (age = 5)"),
           std::string("age != 5"));
}

TEST(Rewriter, PushdownNotDeMorgan) {
  CHECK_EQ(
      rewrite_where("SELECT * FROM users WHERE NOT (age > 5 AND name = 'x')"),
      std::string("(age <= 5 OR name != x)"));
  CHECK_EQ(
      rewrite_where("SELECT * FROM users WHERE NOT (age > 5 OR name = 'x')"),
      std::string("(age <= 5 AND name != x)"));
}

TEST(Rewriter, PushdownNotRemovesDoubleNegation) {
  CHECK_EQ(rewrite_where("SELECT * FROM users WHERE NOT (NOT (age > 5))"),
           std::string("age > 5"));
}

TEST(Rewriter, PushdownNotNullPredicate) {
  CHECK_EQ(rewrite_where("SELECT * FROM users WHERE NOT (age IS NULL)"),
           std::string("age IS NOT NULL"));
  CHECK_EQ(rewrite_where("SELECT * FROM users WHERE NOT (age IS NOT NULL)"),
           std::string("age IS NULL"));
}

TEST(Rewriter, PushdownNotInUsesNotInFlag) {
  // InCondition 自带 is_not_in：NOT (x IN ..) 与 x NOT IN .. 三值语义一致
  CHECK_EQ(rewrite_where("SELECT * FROM users WHERE NOT (age IN (1, 2))"),
           std::string("age NOT IN (1, 2)"));
}

TEST(Rewriter, FlatternsRightNestedAnd) {
  CHECK_EQ(rewrite_where("SELECT * FROM users WHERE age > 1 AND "
                         "(name = 'x' AND age < 9)"),
           std::string("((age > 1 AND name = x) AND age < 9)"));
  CHECK_EQ(rewrite_where("SELECT * FROM users WHERE age = 1 OR "
                         "(name = 'x' OR age = 9)"),
           std::string("(age IN (1, 9) OR name = x)"));
}

TEST(Rewriter, DeduplicatesSameCondition) {
  CHECK_EQ(rewrite_where("SELECT * FROM users WHERE age > 5 AND age > 5"),
           std::string("age > 5"));
  CHECK_EQ(rewrite_where("SELECT * FROM users WHERE age > 5 OR age > 5"),
           std::string("age > 5"));
  CHECK_EQ(rewrite_where("SELECT * FROM users WHERE (age > 5 AND name = 'x') "
                         "OR (age > 5 AND name = 'x')"),
           std::string("(age > 5 AND name = x)"));
}

TEST(Rewriter, AbsorbsRedundantBranch) {
  // A OR (A AND B) -> A
  CHECK_EQ(rewrite_where("SELECT * FROM users WHERE age > 5 OR "
                         "(age > 5 AND name = 'x')"),
           std::string("age > 5"));
  // A AND (A OR B) -> A
  CHECK_EQ(rewrite_where("SELECT * FROM users WHERE (age > 5 OR name = 'x') "
                         "AND age > 5"),
           std::string("age > 5"));
}

TEST(Rewriter, OrOfEqualityBecomesIn) {
  CHECK_EQ(rewrite_where("SELECT * FROM users WHERE age = 1 OR age = 2"),
           std::string("age IN (1, 2)"));
  CHECK_EQ(rewrite_where("SELECT * FROM users WHERE age = 1 OR age = 2 OR "
                         "name = 'x'"),
           std::string("(age IN (1, 2) OR name = x)"));
  // 不同列不合并
  CHECK_EQ(rewrite_where("SELECT * FROM users WHERE age = 1 OR name = 'x'"),
           std::string("(age = 1 OR name = x)"));
}

TEST(Rewriter, FoldsInListConstants) {
  CHECK_EQ(rewrite_where("SELECT * FROM users WHERE age IN (1, 1, 2)"),
           std::string("age IN (1, 2)"));
  CHECK_EQ(rewrite_where("SELECT * FROM users WHERE age IN (7)"),
           std::string("age = 7"));
  CHECK_EQ(rewrite_where("SELECT * FROM users WHERE age NOT IN (7)"),
           std::string("age != 7"));
}

TEST(Rewriter, LeavesDdlAndInsertUntouched) {
  plan::QueryRewriter rewriter;

  auto insert =
      build_query("INSERT INTO users (id, name, age) VALUES (1, 'a', 20)");
  CHECK(insert.has_value());
  if (insert.has_value()) {
    auto rewritten = rewriter.rewrite(*insert);
    CHECK(rewritten.has_value());
    if (rewritten.has_value()) {
      CHECK(rewritten->is_insert());
      CHECK_EQ(rewritten->to_string(), insert->to_string());
    }
  }

  auto ddl =
      build_query("CREATE TABLE t (id INT PRIMARY KEY, name VARCHAR(8))");
  CHECK(ddl.has_value());
  if (ddl.has_value()) {
    auto rewritten = rewriter.rewrite(*ddl);
    CHECK(rewritten.has_value());
    if (rewritten.has_value()) {
      CHECK(rewritten->is_create_table());
      CHECK_EQ(rewritten->to_string(), ddl->to_string());
    }
  }
}

TEST(Rewriter, DoesNotModifyInputQuery) {
  auto query = build_query("SELECT * FROM users WHERE NOT (age > 5)");
  CHECK(query.has_value());
  if (!query.has_value()) {
    return;
  }
  const std::string before = query->to_string();

  plan::QueryRewriter rewriter;
  auto rewritten = rewriter.rewrite(*query);
  CHECK(rewritten.has_value());
  CHECK_EQ(query->to_string(), before); // 原 Query 不被改动
  if (rewritten.has_value()) {
    CHECK(rewritten->to_string().find("age <= 5") != std::string::npos);
  }
}

TEST(Rewriter, SelectWithoutWhereStaysWithoutWhere) {
  CHECK_EQ(rewrite_where("SELECT * FROM users"), std::string("-"));
}
