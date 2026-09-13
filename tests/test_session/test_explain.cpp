// tests/test_session/test_explain.cpp
//
// EXPLAIN：由语法层识别（parser/sql.y 的 explain_stmt），session 拆掉前缀后
// 只跑 pipeline 到计划树为止（**不执行**），把计划文本当成一个
// **单列结果集**（列名 "QUERY PLAN"，一行一段）交给客户端 ——
// 所以它和别的语句走同一条 execute()/next()/close() 路径。
#include "test_framework.h"

#include <memory>
#include <string>

#include "session_test_util.h"

namespace {

// 建库建表塞数据，然后 EXPLAIN 一条语句
std::string explain_sql(session::Session &session, const std::string &sql,
                        bool *ok = nullptr) {
  auto result = sess_test::explain(session, sql);
  if (ok != nullptr) {
    *ok = result.ok;
  }
  return result.text;
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

TEST(Explain, ResultIsASingleColumnOfPlanLines) {
  auto engine = sess_test::open_engine();
  session::Session session(engine);
  CHECK(sess_test::bootstrap(session, 3));

  auto result = sess_test::explain(session, "EXPLAIN SELECT * FROM users");
  CHECK(result.ok);
  CHECK_EQ(result.columns.size(), size_t{1});
  if (result.columns.size() == 1) {
    CHECK_EQ(result.columns[0], std::string("QUERY PLAN"));
  }
  // 一行一个算子，拼接回来就是 plan_tree_to_string 的内容
  CHECK(!result.rows.empty());
  for (const sql::Row &row : result.rows) {
    CHECK_EQ(row.size(), size_t{1});
  }
  CHECK(contains(result.text, "FullScan(users pk=id INT"));
  // 每行都是一段单行文本（换行不会留在单元格里，表格才对得齐）
  for (const sql::Row &row : result.rows) {
    CHECK(row[0].as_str().find('\n') == std::string::npos);
  }
}

TEST(Explain, KeywordIsCaseInsensitiveAndPlainSelectStillExecutes) {
  auto engine = sess_test::open_engine();
  session::Session session(engine);
  CHECK(sess_test::bootstrap(session, 3));

  bool ok = false;
  const std::string upper =
      explain_sql(session, "EXPLAIN SELECT * FROM users", &ok);
  CHECK(ok);
  const std::string lower =
      explain_sql(session, "  explain   select * from users  ", &ok);
  CHECK(ok);
  CHECK_EQ(upper, lower);

  // 不加 EXPLAIN 就是"真执行"：拿到的是数据行，不是计划
  const auto ids =
      sess_test::first_column_ints(session, "SELECT * FROM users", &ok);
  CHECK(ok);
  CHECK_EQ(ids.size(), size_t{3});
  // EXPLAIN 不执行：表里还是 3 行
  explain_sql(session, "EXPLAIN DELETE FROM users", &ok);
  CHECK(ok);
  const auto after =
      sess_test::first_column_ints(session, "SELECT id FROM users", &ok);
  CHECK(ok);
  CHECK_EQ(after.size(), size_t{3});
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

  auto result = sess_test::explain(
      session, "EXPLAIN CREATE TABLE t (id INT PRIMARY KEY)");
  CHECK(!result.ok);
  CHECK(result.error.code == session::SessionErrorCode::NOT_SUPPORTED);
  // USE 同理
  auto use = sess_test::explain(session, "EXPLAIN USE shop");
  CHECK(!use.ok);
  CHECK(use.error.code == session::SessionErrorCode::NOT_SUPPORTED);
  // 事务控制语句也没有计划树（语法层认得它，所以提示更具体）
  auto begin = sess_test::explain(session, "EXPLAIN BEGIN");
  CHECK(!begin.ok);
  CHECK(begin.error.code == session::SessionErrorCode::NOT_SUPPORTED);
  CHECK(begin.error.message.find("BEGIN") != std::string::npos);
}

TEST(Explain, ErrorsKeepTheirKind) {
  auto engine = sess_test::open_engine();
  session::Session session(engine);
  CHECK(sess_test::bootstrap(session, 1));

  // 表不存在 -> VALIDATE_ERROR；位置是**原文里的列号**（前缀不再需要换算）
  auto missing = sess_test::explain(session, "EXPLAIN SELECT * FROM missing");
  CHECK(!missing.ok);
  CHECK(missing.error.code == session::SessionErrorCode::VALIDATE_ERROR);
  CHECK(sspan_valid(missing.error.span));
  CHECK(missing.error.highlight(false).find("missing") != std::string::npos);

  // 语法错误 -> PARSE_ERROR
  auto syntax = sess_test::explain(session, "EXPLAIN SELCT 1");
  CHECK(!syntax.ok);
  CHECK(syntax.error.code == session::SessionErrorCode::PARSE_ERROR);

  // 光有 EXPLAIN 没有语句 -> 语法层报缺语句
  auto empty = sess_test::explain(session, "EXPLAIN");
  CHECK(!empty.ok);
  CHECK(empty.error.code == session::SessionErrorCode::PARSE_ERROR);
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

TEST(Explain, AnalyzeNeedsTheExplicitKeyword) {
  auto engine = sess_test::open_engine();
  session::Session session(engine);
  CHECK(sess_test::bootstrap(session, 2));

  // 统计只来自语法层的 ANALYZE 关键字：EXPLAIN 后面不写就只能是估算
  auto plain = sess_test::explain(session, "EXPLAIN SELECT id FROM users");
  CHECK(plain.ok);
  CHECK(!contains(plain.text, "[rows="));

  auto analyzed =
      sess_test::explain(session, "EXPLAIN ANALYZE SELECT id FROM users");
  CHECK(analyzed.ok);
  CHECK(contains(analyzed.text, "[rows="));

  // ANALYZE 只是 EXPLAIN 的修饰词，单独出现不是语句
  auto alone = sess_test::explain(session, "ANALYZE SELECT id FROM users");
  CHECK(!alone.ok);
  CHECK(alone.error.code == session::SessionErrorCode::PARSE_ERROR);
}

TEST(Explain, AnalyzeRejectsWriteStatementsAndLeavesDataAlone) {
  auto engine = sess_test::open_engine();
  session::Session session(engine);
  CHECK(sess_test::bootstrap(session, 3));

  for (const std::string &sql : {"EXPLAIN ANALYZE DELETE FROM users",
                                 "EXPLAIN ANALYZE UPDATE users SET age = 1",
                                 "EXPLAIN ANALYZE INSERT INTO users (id, name, "
                                 "age) VALUES (9, 'z', 1)"}) {
    auto result = sess_test::explain(session, sql);
    CHECK(!result.ok);
    CHECK(result.error.code == session::SessionErrorCode::NOT_SUPPORTED);
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

  auto result =
      sess_test::explain(session, "EXPLAIN ANALYZE SELECT * FROM users");
  CHECK(!result.ok);
  CHECK(result.error.code == session::SessionErrorCode::EXECUTE_ERROR);
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
