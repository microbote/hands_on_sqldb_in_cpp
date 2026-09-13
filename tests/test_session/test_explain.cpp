// tests/test_session/test_explain.cpp
//
// EXPLAIN：只跑 pipeline 到计划树为止，不执行；输出就是 plan_tree_to_string。
#include "test_framework.h"

#include <memory>
#include <string>

#include "session_test_util.h"

namespace {

// 建库建表塞数据，然后 EXPLAIN 一条语句
std::string explain_sql(session::Session &session, const std::string &sql,
                        bool *ok = nullptr) {
  auto result = session.explain(sql);
  if (!result.has_value()) {
    if (ok != nullptr) {
      *ok = false;
    }
    return result.error().to_string();
  }
  if (ok != nullptr) {
    *ok = true;
  }
  return *result;
}

bool contains(const std::string &text, const std::string &needle) {
  return text.find(needle) != std::string::npos;
}

} // namespace

TEST(Explain, FullScanForUnfilteredSelect) {
  auto engine = sess_test::open_engine();
  session::Session session(engine);
  CHECK(sess_test::bootstrap(session, 3));

  bool ok = false;
  const std::string plan =
      explain_sql(session, "EXPLAIN SELECT * FROM users", &ok);
  CHECK(ok);
  CHECK(contains(plan, "FullScan(users pk=id INT"));
}

TEST(Explain, IndexScanForPkPoint) {
  auto engine = sess_test::open_engine();
  session::Session session(engine);
  CHECK(sess_test::bootstrap(session, 3));

  bool ok = false;
  const std::string plan =
      explain_sql(session, "EXPLAIN SELECT * FROM users WHERE id = 5", &ok);
  CHECK(ok);
  CHECK(contains(plan, "IndexScan(users pk=id INT, [5, 5]"));
}

TEST(Explain, RangeUnionForSparseInList) {
  auto engine = sess_test::open_engine();
  session::Session session(engine);
  // 表要足够大：点集相对表太大时成本模型会退化成全表扫（见 CostModel 用例）
  CHECK(sess_test::bootstrap(session, 100));

  bool ok = false;
  const std::string plan = explain_sql(
      session, "EXPLAIN SELECT * FROM users WHERE id IN (1, 3, 5)", &ok);
  CHECK(ok);
  CHECK(contains(plan, "RangeUnion"));
  CHECK(contains(plan, "[[1, 1], [3, 3], [5, 5]]"));
}

TEST(Explain, ExclusionShowsUpOnRangeUnion) {
  auto engine = sess_test::open_engine();
  session::Session session(engine);
  CHECK(sess_test::bootstrap(session, 3));

  bool ok = false;
  const std::string plan =
      explain_sql(session, "EXPLAIN SELECT * FROM users WHERE id != 5", &ok);
  CHECK(ok);
  CHECK(contains(plan, "exclude={5}"));
  // 兜底的过滤条件也在计划里（正确性不靠"跳点提示"）
  CHECK(contains(plan, "Filter(id != 5)"));
}

TEST(Explain, OrderByPrimaryKeyNeedsNoSortAndGoesReverse) {
  auto engine = sess_test::open_engine();
  session::Session session(engine);
  CHECK(sess_test::bootstrap(session, 3));

  bool ok = false;
  const std::string plan = explain_sql(
      session, "EXPLAIN SELECT * FROM users ORDER BY id DESC LIMIT 2", &ok);
  CHECK(ok);
  CHECK(contains(plan, "desc)"));  // 反向扫
  CHECK(!contains(plan, "Sort(")); // 没有排序节点
  CHECK(contains(plan, "Limit(limit=2 offset=0)"));
}

TEST(Explain, OrderByNonPrimaryKeyBecomesTopN) {
  auto engine = sess_test::open_engine();
  session::Session session(engine);
  CHECK(sess_test::bootstrap(session, 3));

  bool ok = false;
  const std::string plan = explain_sql(
      session, "EXPLAIN SELECT id FROM users ORDER BY age LIMIT 2 OFFSET 1",
      &ok);
  CHECK(ok);
  CHECK(contains(plan, "Project([id])"));
  CHECK(contains(plan, "TopN(order_by=[age ASC] n=3)"));
  CHECK(contains(plan, "Limit(limit=2 offset=1)"));
}

TEST(Explain, WriteStatementsShowTheirScanChain) {
  auto engine = sess_test::open_engine();
  session::Session session(engine);
  CHECK(sess_test::bootstrap(session, 3));

  bool ok = false;
  std::string plan = explain_sql(
      session, "EXPLAIN UPDATE users SET age = 1 WHERE id = 3", &ok);
  CHECK(ok);
  CHECK(contains(plan, "Update(users pk=id)"));
  CHECK(contains(plan, "IndexScan(users pk=id INT, [3, 3]"));

  plan = explain_sql(session, "EXPLAIN DELETE FROM users", &ok);
  CHECK(ok);
  CHECK(contains(plan, "Delete(users pk=id)"));
  CHECK(contains(plan, "FullScan"));

  plan = explain_sql(
      session, "EXPLAIN INSERT INTO users (id, name, age) VALUES (9, 'z', 1)",
      &ok);
  CHECK(ok);
  CHECK(contains(plan, "Insert(users pk=id)"));
}

TEST(Explain, PrefixIsOptionalAndCaseInsensitive) {
  auto engine = sess_test::open_engine();
  session::Session session(engine);
  CHECK(sess_test::bootstrap(session, 3));

  bool ok = false;
  const std::string with_prefix =
      explain_sql(session, "EXPLAIN SELECT * FROM users", &ok);
  CHECK(ok);
  const std::string lowercase =
      explain_sql(session, "  explain   select * from users  ", &ok);
  CHECK(ok);
  const std::string without_prefix =
      explain_sql(session, "SELECT * FROM users", &ok);
  CHECK(ok);
  CHECK_EQ(with_prefix, lowercase);
  CHECK_EQ(with_prefix, without_prefix);
}

TEST(Explain, DoesNotExecuteTheStatement) {
  auto engine = sess_test::open_engine();
  session::Session session(engine);
  CHECK(sess_test::bootstrap(session, 3));

  bool ok = false;
  explain_sql(session, "EXPLAIN DELETE FROM users", &ok);
  CHECK(ok);
  explain_sql(session, "EXPLAIN UPDATE users SET age = 99", &ok);
  CHECK(ok);
  explain_sql(session,
              "EXPLAIN INSERT INTO users (id, name, age) VALUES (9, 'z', 1)",
              &ok);
  CHECK(ok);

  // 表里还是原来的 3 行，值也没被改
  const auto ids =
      sess_test::first_column_ints(session, "SELECT id FROM users", &ok);
  CHECK(ok);
  CHECK_EQ(ids.size(), size_t{3});
  const auto ages = sess_test::first_column_ints(
      session, "SELECT age FROM users WHERE id = 1", &ok);
  CHECK(ok);
  CHECK_EQ(ages.size(), size_t{1});
  if (ages.size() == 1) {
    CHECK_EQ(ages[0], int64_t{10}); // bootstrap 里 age = id*10
  }
}

TEST(Explain, DdlIsNotSupported) {
  auto engine = sess_test::open_engine();
  session::Session session(engine);
  CHECK(sess_test::bootstrap(session, 1));

  auto result = session.explain("EXPLAIN CREATE TABLE t (id INT PRIMARY KEY)");
  CHECK(!result.has_value());
  if (!result.has_value()) {
    CHECK(result.error().code == session::SessionErrorCode::NOT_SUPPORTED);
  }
  // USE 同理
  auto use = session.explain("EXPLAIN USE shop");
  CHECK(!use.has_value());
}

TEST(Explain, ErrorsKeepTheirKind) {
  auto engine = sess_test::open_engine();
  session::Session session(engine);
  CHECK(sess_test::bootstrap(session, 1));

  // 表不存在 -> VALIDATE_ERROR，且位置换算回"原始文本"（含 EXPLAIN 前缀）
  auto missing = session.explain("EXPLAIN SELECT * FROM missing");
  CHECK(!missing.has_value());
  if (!missing.has_value()) {
    CHECK(missing.error().code == session::SessionErrorCode::VALIDATE_ERROR);
    CHECK(sspan_valid(missing.error().span));
    CHECK(missing.error().highlight(false).find("missing") !=
          std::string::npos);
  }

  // 语法错误 -> PARSE_ERROR
  auto syntax = session.explain("EXPLAIN SELCT 1");
  CHECK(!syntax.has_value());
  if (!syntax.has_value()) {
    CHECK(syntax.error().code == session::SessionErrorCode::PARSE_ERROR);
  }

  // 空语句
  auto empty = session.explain("EXPLAIN");
  CHECK(!empty.has_value());
  if (!empty.has_value()) {
    CHECK(empty.error().code == session::SessionErrorCode::EMPTY_SQL);
  }
}

// ============================================================
// EXPLAIN ANALYZE：真的执行，报每个算子的实际行数与耗时
// ============================================================
TEST(Explain, AnalyzeShowsActualRowCounts) {
  auto engine = sess_test::open_engine();
  session::Session session(engine);
  CHECK(sess_test::bootstrap(session, 5));

  bool ok = false;
  const std::string plan = explain_sql(
      session, "EXPLAIN ANALYZE SELECT id FROM users WHERE id >= 4", &ok);
  CHECK(ok);
  CHECK(contains(plan, "IndexScan(users pk=id INT, [4, +∞), asc)  [rows=2 "));
  CHECK(contains(plan, "time="));
  CHECK(contains(plan, "(2 rows in result)"));
}

TEST(Explain, AnalyzeExposesEarlyStop) {
  auto engine = sess_test::open_engine();
  session::Session session(engine);
  CHECK(sess_test::bootstrap(session, 5));

  bool ok = false;
  // 不带 LIMIT：扫描真的读了 5 行
  std::string plan =
      explain_sql(session, "EXPLAIN ANALYZE SELECT id FROM users", &ok);
  CHECK(ok);
  CHECK(contains(plan, "FullScan(users pk=id INT, asc)  [rows=5 "));

  // 带 LIMIT：扫描只读 2 行就停了（早停可观测）
  plan = explain_sql(
      session, "EXPLAIN ANALYZE SELECT id FROM users ORDER BY id LIMIT 2", &ok);
  CHECK(ok);
  CHECK(contains(plan, "FullScan(users pk=id INT, asc)  [rows=2 "));
  CHECK(contains(plan, "Limit(limit=2 offset=0)  [rows=2 "));
}

TEST(Explain, AnalyzeShowsFilterSelectivity) {
  auto engine = sess_test::open_engine();
  session::Session session(engine);
  CHECK(sess_test::bootstrap(session, 5)); // age = 10,20,30,40,50

  bool ok = false;
  const std::string plan = explain_sql(
      session, "EXPLAIN ANALYZE SELECT id FROM users WHERE age >= 30", &ok);
  CHECK(ok);
  // 全表扫 5 行，过滤后剩 3 行（30/40/50）
  CHECK(contains(plan, "FullScan(users pk=id INT, asc)  [rows=5 "));
  CHECK(contains(plan, "Filter(age >= 30)  [rows=3 "));
  CHECK(contains(plan, "(3 rows in result)"));
}

TEST(Explain, AnalyzeAcceptsExplicitFlagWithoutPrefix) {
  auto engine = sess_test::open_engine();
  session::Session session(engine);
  CHECK(sess_test::bootstrap(session, 2));

  auto analyzed = session.explain("SELECT id FROM users", /*analyze=*/true);
  CHECK(analyzed.has_value());
  if (analyzed.has_value()) {
    CHECK(contains(*analyzed, "[rows="));
  }
  // 不带 analyze 时不给统计
  auto plain = session.explain("SELECT id FROM users");
  CHECK(plain.has_value());
  if (plain.has_value()) {
    CHECK(!contains(*plain, "[rows="));
  }
}

TEST(Explain, AnalyzeRejectsWriteStatementsAndLeavesDataAlone) {
  auto engine = sess_test::open_engine();
  session::Session session(engine);
  CHECK(sess_test::bootstrap(session, 3));

  for (const std::string &sql : {"EXPLAIN ANALYZE DELETE FROM users",
                                 "EXPLAIN ANALYZE UPDATE users SET age = 1",
                                 "EXPLAIN ANALYZE INSERT INTO users (id, name, "
                                 "age) VALUES (9, 'z', 1)"}) {
    auto result = session.explain(sql);
    CHECK(!result.has_value());
    if (!result.has_value()) {
      CHECK(result.error().code == session::SessionErrorCode::NOT_SUPPORTED);
    }
  }
  // 数据没被动过
  bool ok = false;
  const auto ids =
      sess_test::first_column_ints(session, "SELECT id FROM users", &ok);
  CHECK(ok);
  CHECK_EQ(ids.size(), size_t{3});
}

TEST(Explain, AnalyzeReportsExecutionErrors) {
  auto engine = sess_test::open_engine();
  session::Session session(engine);
  CHECK(sess_test::bootstrap(session, 2));

  // 把一行数据写坏：ANALYZE 会真的执行，所以必须把错误报出来
  auto table = session.catalog().open_table(sql::Identifier("shop"),
                                            sql::Identifier("users"));
  CHECK(table.has_value());
  if (table.has_value()) {
    const std::string key =
        table->encode_key(sql::Value(int64_t{2}, sql::DataType::INT));
    CHECK(engine->put(key, "garbage") == kv::Status::OK);
  }

  auto result = session.explain("EXPLAIN ANALYZE SELECT * FROM users");
  CHECK(!result.has_value());
  if (!result.has_value()) {
    CHECK(result.error().code == session::SessionErrorCode::EXECUTE_ERROR);
  }
}

TEST(Explain, AnalyzeOnEmptyResultStillPrintsThePlan) {
  auto engine = sess_test::open_engine();
  session::Session session(engine);
  CHECK(sess_test::bootstrap(session, 3));

  bool ok = false;
  const std::string plan = explain_sql(
      session, "EXPLAIN ANALYZE SELECT * FROM users WHERE id > 5 AND id < 3",
      &ok);
  CHECK(ok);
  CHECK(contains(plan, "[rows=0 "));
  CHECK(contains(plan, "(0 rows in result)"));
}
