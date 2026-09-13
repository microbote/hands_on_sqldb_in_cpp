// tests/test_session/test_cost.cpp
//
// 极简成本模型（占位）：目前唯一的决策点是"稀疏点集要不要下推"。
//
//   k 个点查 ≈ k 次 seek + k 行；全表扫 ≈ N 行。
//   点集相对表太大（这里约 k > N/2）-> 走全表扫 + 过滤更便宜。
#include "test_framework.h"

#include <memory>
#include <string>

#include "session_test_util.h"

namespace {

bool contains(const std::string &text, const std::string &needle) {
  return text.find(needle) != std::string::npos;
}

} // namespace

TEST(CostModel, PointLookupsLoseOnTinyTable) {
  auto engine = sess_test::open_engine();
  session::Session session(engine);
  CHECK(sess_test::bootstrap(session, 3)); // 3 行表

  // 2 个点：2*(seek+row) = 4 > 3 行 -> 退化成全表扫，谓词放回过滤条件
  auto planned =
      session.explain("EXPLAIN SELECT * FROM users WHERE id IN (1, 3)");
  CHECK(planned.has_value());
  if (!planned.has_value()) {
    return;
  }
  CHECK(contains(*planned, "FullScan"));
  CHECK(!contains(*planned, "RangeUnion"));
  CHECK(contains(*planned, "Filter(id IN (1, 3))")); // 正确性靠它兜底
}

TEST(CostModel, PointLookupsWinOnLargeTable) {
  auto engine = sess_test::open_engine();
  session::Session session(engine);
  CHECK(sess_test::bootstrap(session, 100)); // 100 行表

  auto planned =
      session.explain("EXPLAIN SELECT * FROM users WHERE id IN (1, 3)");
  CHECK(planned.has_value());
  if (!planned.has_value()) {
    return;
  }
  CHECK(contains(*planned, "RangeUnion"));
  CHECK(!contains(*planned, "Filter("));
}

TEST(CostModel, FallbackKeepsResultsIdentical) {
  // 同一个查询、两种表大小 -> 计划不同（全表扫 vs 点查），结果必须一样
  const std::string sql = "SELECT id FROM users WHERE id IN (1, 3)";

  auto small_engine = sess_test::open_engine();
  session::Session small(small_engine);
  CHECK(sess_test::bootstrap(small, 3));
  bool ok = false;
  const auto small_ids = sess_test::first_column_ints(small, sql, &ok);
  CHECK(ok);

  auto big_engine = sess_test::open_engine();
  session::Session big(big_engine);
  CHECK(sess_test::bootstrap(big, 100));
  const auto big_ids = sess_test::first_column_ints(big, sql, &ok);
  CHECK(ok);

  CHECK_EQ(small_ids.size(), size_t{2});
  CHECK_EQ(big_ids.size(), size_t{2});
  CHECK_EQ(small_ids, big_ids);
}

TEST(CostModel, PlansCarryEstimatedCost) {
  auto engine = sess_test::open_engine();
  session::Session session(engine);
  CHECK(sess_test::bootstrap(session, 100));

  auto planned = session.explain("EXPLAIN SELECT id FROM users WHERE id = 5");
  CHECK(planned.has_value());
  if (planned.has_value()) {
    CHECK(contains(*planned, "cost="));
  }
  // ANALYZE 输出里同时有估算成本与实际行数
  auto analyzed =
      session.explain("EXPLAIN ANALYZE SELECT id FROM users WHERE id = 5");
  CHECK(analyzed.has_value());
  if (analyzed.has_value()) {
    CHECK(contains(*analyzed, "cost="));
    CHECK(contains(*analyzed, "[rows=1 "));
  }
}

TEST(CostModel, RowCountIsMaintainedByWrites) {
  auto engine = sess_test::open_engine();
  session::Session session(engine);
  CHECK(sess_test::bootstrap(session, 0)); // 建库建表，0 行

  auto stats = session.catalog().table_stats(sql::Identifier("shop"),
                                             sql::Identifier("users"));
  CHECK(stats.has_value());
  if (stats.has_value()) {
    CHECK_EQ(stats->row_count, int64_t{0});
  }

  session::SessionError error;
  for (int id = 1; id <= 5; ++id) {
    CHECK(sess_test::run(session,
                         "INSERT INTO users (id, name, age) VALUES (" +
                             std::to_string(id) + ", 'a', 1)",
                         &error) != nullptr);
  }
  CHECK(sess_test::run(session, "DELETE FROM users WHERE id IN (1, 2)",
                       &error) != nullptr);
  CHECK(sess_test::run(session, "UPDATE users SET age = 2 WHERE id = 3",
                       &error) != nullptr);

  stats = session.catalog().table_stats(sql::Identifier("shop"),
                                        sql::Identifier("users"));
  CHECK(stats.has_value());
  if (stats.has_value()) {
    CHECK_EQ(stats->row_count, int64_t{3}); // +5 -2，UPDATE 不改行数
  }
}
