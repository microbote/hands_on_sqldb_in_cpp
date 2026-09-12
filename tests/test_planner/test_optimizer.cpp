// tests/test_planner/test_optimizer.cpp
//
// Optimizer：主键条件 -> 有序区间（ranges）+ 跳点（exclude_keys）+ 残余过滤。
// 残余过滤。
#include "test_framework.h"

#include <optional>
#include <string>
#include <vector>

#include "planner/optimizer.h"
#include "planner_test_util.h"

namespace {

using plantest::build_query;

// 优化一条 SQL；失败时返回 nullopt（调用方 CHECK）
std::optional<plan::OptimizedQuery> optimize_sql(const sql::Catalog &catalog,
                                                 const std::string &sql) {
  auto query = build_query(sql);
  if (!query.has_value()) {
    return std::nullopt;
  }
  plan::Optimizer optimizer(catalog);
  auto optimized = optimizer.optimize(*query);
  if (!optimized.has_value()) {
    return std::nullopt;
  }
  return std::optional<plan::OptimizedQuery>(std::move(*optimized));
}

} // namespace

// ============================================================
// 点查询 / IN 列表
// ============================================================
TEST(Optimizer, EqualityBecomesPointQuery) {
  MemoryCatalog catalog;
  plantest::setup_catalog(catalog);
  auto oq = optimize_sql(catalog, "SELECT * FROM users WHERE id = 5");
  CHECK(oq.has_value());
  if (!oq.has_value()) {
    return;
  }
  CHECK(oq->is_point_query);
  CHECK_EQ(oq->ranges.size(), size_t{1});
  CHECK(oq->ranges[0].is_point());
  CHECK(oq->ranges[0].contains(sql::Value(int64_t{5})));
  CHECK(oq->remaining_filter == nullptr); // 完全下推，执行器不用再过滤
  CHECK_EQ(oq->primary_key.str(), std::string("id"));
  CHECK(oq->point_value().has_value());
  CHECK_EQ(oq->point_value()->get_int(-1), int64_t{5});
}

TEST(Optimizer, ContiguousInListCoalescesIntoOneRange) {
  MemoryCatalog catalog;
  plantest::setup_catalog(catalog);
  // 连续的点合并成一段：IN (1,2,3) 等价于 [1,3]，1 次 seek 而不是 3 次
  auto oq = optimize_sql(catalog, "SELECT * FROM users WHERE id IN (1, 2, 3)");
  CHECK(oq.has_value());
  if (!oq.has_value()) {
    return;
  }
  CHECK_EQ(oq->ranges.size(), size_t{1});
  CHECK_EQ(oq->ranges[0].to_string(), std::string("[1, 3]"));
  CHECK(!oq->is_point_query);
  CHECK(oq->remaining_filter == nullptr);
}

TEST(Optimizer, DisjointPointsStaySeparateAndSorted) {
  MemoryCatalog catalog;
  plantest::setup_catalog(catalog);
  auto oq = optimize_sql(catalog, "SELECT * FROM users WHERE id IN (5, 1, 3)");
  CHECK(oq.has_value());
  if (!oq.has_value()) {
    return;
  }
  CHECK_EQ(oq->ranges.size(), size_t{3});
  // 区间恒按 low_key 升序（= 主键升序）
  CHECK_EQ(oq->ranges[0].to_string(), std::string("[1, 1]"));
  CHECK_EQ(oq->ranges[1].to_string(), std::string("[3, 3]"));
  CHECK_EQ(oq->ranges[2].to_string(), std::string("[5, 5]"));
}

TEST(Optimizer, OrOfEqualityBecomesPointRanges) {
  MemoryCatalog catalog;
  plantest::setup_catalog(catalog);
  auto oq = optimize_sql(catalog, "SELECT * FROM users WHERE id = 1 OR id = 5");
  CHECK(oq.has_value());
  if (!oq.has_value()) {
    return;
  }
  CHECK_EQ(oq->ranges.size(), size_t{2});
  CHECK(oq->remaining_filter == nullptr);
}

TEST(Optimizer, RangeConditionBecomesInterval) {
  MemoryCatalog catalog;
  plantest::setup_catalog(catalog);
  auto oq =
      optimize_sql(catalog, "SELECT * FROM users WHERE id >= 3 AND id < 7");
  CHECK(oq.has_value());
  if (!oq.has_value()) {
    return;
  }
  CHECK_EQ(oq->ranges.size(), size_t{1});
  CHECK_EQ(oq->ranges[0].to_string(), std::string("[3, 7)"));
  CHECK(oq->remaining_filter == nullptr);
}

TEST(Optimizer, BoundedRangeBecomesTwoHalfIntervals) {
  MemoryCatalog catalog;
  plantest::setup_catalog(catalog);
  auto oq =
      optimize_sql(catalog, "SELECT * FROM users WHERE id < 10 OR id > 100");
  CHECK(oq.has_value());
  if (!oq.has_value()) {
    return;
  }
  CHECK_EQ(oq->ranges.size(), size_t{2});
  CHECK(oq->remaining_filter == nullptr);
}

TEST(Optimizer, RangeCoveringSingleValueIsPointQuery) {
  MemoryCatalog catalog;
  plantest::setup_catalog(catalog);
  auto oq =
      optimize_sql(catalog, "SELECT * FROM users WHERE id >= 5 AND id <= 5");
  CHECK(oq.has_value());
  if (!oq.has_value()) {
    return;
  }
  CHECK(oq->is_point_query);
  CHECK_EQ(oq->ranges.size(), size_t{1});
  CHECK(oq->ranges[0].is_point());
  CHECK(oq->ranges[0].contains(sql::Value(int64_t{5})));
}

TEST(Optimizer, UpperBoundedRangeExcludesNull) {
  MemoryCatalog catalog;
  plantest::setup_catalog(catalog);
  // 比较谓词不匹配 NULL：下界是"第一个非 NULL 值"
  auto oq = optimize_sql(catalog, "SELECT * FROM users WHERE id <= 7");
  CHECK(oq.has_value());
  if (!oq.has_value()) {
    return;
  }
  CHECK_EQ(oq->ranges.size(), size_t{1});
  CHECK_EQ(oq->ranges[0].to_string(), std::string("(NULL, 7]"));
}

// ============================================================
// NULL 语义（三值逻辑）
// ============================================================
TEST(Optimizer, IsNullBecomesNullPoint) {
  MemoryCatalog catalog;
  plantest::setup_catalog(catalog);
  auto oq = optimize_sql(catalog, "SELECT * FROM users WHERE id IS NULL");
  CHECK(oq.has_value());
  if (!oq.has_value()) {
    return;
  }
  CHECK(oq->is_point_query);
  CHECK_EQ(oq->ranges.size(), size_t{1});
  CHECK(oq->ranges[0].is_point());
  CHECK(oq->remaining_filter == nullptr);
  CHECK(oq->point_value().has_value());
  CHECK(oq->point_value()->is_null());
}

TEST(Optimizer, IsNotNullExcludesNull) {
  MemoryCatalog catalog;
  plantest::setup_catalog(catalog);
  auto oq = optimize_sql(catalog, "SELECT * FROM users WHERE id IS NOT NULL");
  CHECK(oq.has_value());
  if (!oq.has_value()) {
    return;
  }
  CHECK_EQ(oq->ranges.size(), size_t{1});
  CHECK_EQ(oq->ranges[0].to_string(), std::string("(NULL, +∞)"));
  CHECK(!oq->ranges[0].contains(sql::Value()));
  CHECK(oq->remaining_filter == nullptr);
}

TEST(Optimizer, CompareWithNullLiteralMatchesNothing) {
  MemoryCatalog catalog;
  plantest::setup_catalog(catalog);
  // id = NULL 在 SQL 里恒为 UNKNOWN -> 一行都不命中（不是 IS NULL）
  auto oq = optimize_sql(catalog, "SELECT * FROM users WHERE id = NULL");
  CHECK(oq.has_value());
  if (!oq.has_value()) {
    return;
  }
  CHECK(oq->is_empty_scan());
  CHECK(!oq->is_point_query);
}

TEST(Optimizer, InListWithNullIgnoresNullElement) {
  MemoryCatalog catalog;
  plantest::setup_catalog(catalog);
  auto oq = optimize_sql(catalog, "SELECT * FROM users WHERE id IN (1, NULL)");
  CHECK(oq.has_value());
  if (!oq.has_value()) {
    return;
  }
  CHECK_EQ(oq->ranges.size(), size_t{1});
  CHECK(oq->ranges[0].contains(sql::Value(int64_t{1})));
  CHECK(!oq->ranges[0].contains(sql::Value()));
}

TEST(Optimizer, ContradictoryRangeIsEmptyScan) {
  MemoryCatalog catalog;
  plantest::setup_catalog(catalog);
  auto oq =
      optimize_sql(catalog, "SELECT * FROM users WHERE id > 5 AND id < 3");
  CHECK(oq.has_value());
  if (!oq.has_value()) {
    return;
  }
  CHECK(oq->is_empty_scan());
  CHECK(!oq->is_all_scan());
}

// ============================================================
// 不可索引 / 不可下推：留在 remaining_filter
// ============================================================
TEST(Optimizer, NotEqualKeepsFilterAndExclusionHint) {
  MemoryCatalog catalog;
  plantest::setup_catalog(catalog);
  auto oq = optimize_sql(catalog, "SELECT * FROM users WHERE id != 5");
  CHECK(oq.has_value());
  if (!oq.has_value()) {
    return;
  }
  CHECK_EQ(oq->exclude_keys.size(), size_t{1});
  CHECK(oq->exclude_keys.contains(sql::Value(int64_t{5})));
  CHECK_EQ(oq->ranges.size(), size_t{1});
  CHECK(oq->ranges[0].is_all());
  CHECK(oq->remaining_filter != nullptr); // 正确性靠过滤，不靠提示
  CHECK(!oq->is_all_scan());              // 有排除点，不是纯全表扫描
  CHECK(oq->needs_index_scan());
}

TEST(Optimizer, NotInKeepsFilterAndExclusionHint) {
  MemoryCatalog catalog;
  plantest::setup_catalog(catalog);
  auto oq = optimize_sql(catalog, "SELECT * FROM users WHERE id NOT IN (1, 2)");
  CHECK(oq.has_value());
  if (!oq.has_value()) {
    return;
  }
  CHECK_EQ(oq->exclude_keys.size(), size_t{2});
  CHECK(oq->remaining_filter != nullptr);
}

TEST(Optimizer, NegatedPkEqualityBecomesNotEqual) {
  MemoryCatalog catalog;
  plantest::setup_catalog(catalog);
  auto oq = optimize_sql(catalog, "SELECT * FROM users WHERE NOT (id = 5)");
  CHECK(oq.has_value());
  if (!oq.has_value()) {
    return;
  }
  CHECK_EQ(oq->exclude_keys.size(), size_t{1});
  CHECK(oq->remaining_filter != nullptr);
}

TEST(Optimizer, NonPrimaryKeyFilterKeepsFullScan) {
  MemoryCatalog catalog;
  plantest::setup_catalog(catalog);
  auto oq = optimize_sql(catalog, "SELECT * FROM users WHERE name = 'x'");
  CHECK(oq.has_value());
  if (!oq.has_value()) {
    return;
  }
  CHECK(oq->is_all_scan());
  CHECK(oq->remaining_filter != nullptr);
}

TEST(Optimizer, OrWithNonPrimaryKeyBranchIsNotPushedDown) {
  MemoryCatalog catalog;
  plantest::setup_catalog(catalog);
  // OR 的一支不是主键条件：整棵 OR 退回过滤（只下推一支会少扫行）
  auto oq =
      optimize_sql(catalog, "SELECT * FROM users WHERE id = 1 OR name = 'x'");
  CHECK(oq.has_value());
  if (!oq.has_value()) {
    return;
  }
  CHECK(oq->is_all_scan());
  CHECK(oq->remaining_filter != nullptr);
}

TEST(Optimizer, PkConditionAndFilterTogether) {
  MemoryCatalog catalog;
  plantest::setup_catalog(catalog);
  auto oq =
      optimize_sql(catalog, "SELECT * FROM users WHERE id = 5 AND name = 'x'");
  CHECK(oq.has_value());
  if (!oq.has_value()) {
    return;
  }
  CHECK(oq->is_point_query);
  CHECK(oq->remaining_filter != nullptr);
  CHECK_EQ(oq->remaining_filter->to_string(), std::string("name = x"));
}

// ============================================================
// 没有 WHERE / 没有主键 / 字符串主键
// ============================================================
TEST(Optimizer, SelectWithoutWhereIsFullScan) {
  MemoryCatalog catalog;
  plantest::setup_catalog(catalog);
  auto oq = optimize_sql(catalog, "SELECT * FROM users");
  CHECK(oq.has_value());
  if (!oq.has_value()) {
    return;
  }
  CHECK(oq->is_all_scan());
  CHECK(oq->remaining_filter == nullptr);
  CHECK(!oq->is_point_query);
}

TEST(Optimizer, DeleteWithoutWhereIsFullScanNotZeroRows) {
  MemoryCatalog catalog;
  plantest::setup_catalog(catalog);
  auto oq = optimize_sql(catalog, "DELETE FROM users");
  CHECK(oq.has_value());
  if (!oq.has_value()) {
    return;
  }
  CHECK(oq->is_all_scan()); // 全表删除，绝不能是"空扫描"
  CHECK(!oq->is_empty_scan());
}

TEST(Optimizer, UpdateWithPkConditionNarrowsScan) {
  MemoryCatalog catalog;
  plantest::setup_catalog(catalog);
  auto oq = optimize_sql(catalog, "UPDATE users SET age = 1 WHERE id = 3");
  CHECK(oq.has_value());
  if (!oq.has_value()) {
    return;
  }
  CHECK(oq->is_point_query);
  CHECK_EQ(oq->ranges.size(), size_t{1});
  CHECK(oq->ranges[0].contains(sql::Value(int64_t{3})));
  CHECK(oq->query.is_update());
}

TEST(Optimizer, TableWithoutPrimaryKeyFallsBackToFullScan) {
  MemoryCatalog catalog;
  plantest::setup_catalog(catalog);
  auto oq = optimize_sql(catalog, "SELECT * FROM logs WHERE message = 'hi'");
  CHECK(oq.has_value());
  if (!oq.has_value()) {
    return;
  }
  CHECK(oq->is_all_scan());
  CHECK(oq->primary_key.empty());
  CHECK(oq->remaining_filter != nullptr);
}

TEST(Optimizer, StringPrimaryKeyUsesStringKeySpace) {
  MemoryCatalog catalog;
  plantest::setup_catalog(catalog);
  auto oq =
      optimize_sql(catalog, "SELECT * FROM accounts WHERE name = 'alice'");
  CHECK(oq.has_value());
  if (!oq.has_value()) {
    return;
  }
  CHECK(oq->is_point_query);
  CHECK_EQ(oq->ranges.size(), size_t{1});
  CHECK(oq->ranges[0].is_point());
  CHECK(oq->ranges[0].contains(sql::Value("alice")));
  CHECK(oq->remaining_filter == nullptr);
}

TEST(Optimizer, StringPrimaryKeyRangeIsOrderedByBytes) {
  MemoryCatalog catalog;
  plantest::setup_catalog(catalog);
  auto oq = optimize_sql(
      catalog, "SELECT * FROM accounts WHERE name >= 'b' AND name < 'd'");
  CHECK(oq.has_value());
  if (!oq.has_value()) {
    return;
  }
  CHECK_EQ(oq->ranges.size(), size_t{1});
  CHECK_EQ(oq->ranges[0].to_string(), std::string("[b, d)"));
  CHECK(oq->ranges[0].contains(sql::Value("bx")));
  CHECK(!oq->ranges[0].contains(sql::Value("d")));
}

// ============================================================
// 错误路径
// ============================================================
TEST(Optimizer, ReportsCatalogNotOpen) {
  MemoryCatalog catalog;
  plantest::setup_catalog(catalog);
  catalog.set_open(false);

  auto query = build_query("SELECT * FROM users WHERE id = 1");
  CHECK(query.has_value());
  if (!query.has_value()) {
    return;
  }
  plan::Optimizer optimizer(catalog);
  auto oq = optimizer.optimize(*query);
  CHECK(!oq.has_value());
  if (!oq.has_value()) {
    CHECK_EQ(static_cast<int>(oq.error().code),
             static_cast<int>(plan::PlanErrorCode::CATALOG_NOT_OPEN));
  }
}

TEST(Optimizer, ReportsTableNotFound) {
  MemoryCatalog catalog;
  plantest::setup_catalog(catalog);
  auto query = build_query("SELECT * FROM missing WHERE id = 1");
  CHECK(query.has_value());
  if (!query.has_value()) {
    return;
  }
  plan::Optimizer optimizer(catalog);
  auto oq = optimizer.optimize(*query);
  CHECK(!oq.has_value());
  if (!oq.has_value()) {
    CHECK_EQ(static_cast<int>(oq.error().code),
             static_cast<int>(plan::PlanErrorCode::TABLE_NOT_FOUND));
  }
}

// ============================================================
// 语义等价（性质测试）：计划保留的行 == 原谓词保留的行
// ============================================================
TEST(Optimizer, PlanKeepsExactlyTheRowsThePredicateKeeps) {
  MemoryCatalog catalog;
  plantest::setup_catalog(catalog);
  plan::Optimizer optimizer(catalog);

  const std::vector<std::string> wheres = {
      "id = 5",
      "id != 5",
      "id > 5",
      "id >= 5",
      "id < 5",
      "id <= 5",
      "id IN (1, 3, 5)",
      "id NOT IN (1, 3)",
      "id IN (1, NULL)",
      "id = NULL",
      "id IS NULL",
      "id IS NOT NULL",
      "id > 5 AND id < 3",
      "id >= 3 AND id <= 7",
      "id = 1 OR id = 7",
      "id < 3 OR id > 7",
      "id != 5 AND id > 2",
      "NOT (id = 5)",
      "NOT (id > 5)",
      "NOT (id IN (1, 2))",
      "id = 5 AND name = 'x'",
      "name = 'x'",
      "id = 1 OR name = 'x'",
      "id IS NULL AND id > 5",
      "id IS NOT NULL AND id = 5",
      "id IS NULL OR id IS NOT NULL",
      "id >= 5 OR id IS NULL",
      "id IN (1, 2) AND id = 2",
      "id IN (1, 2) OR id IN (2, 3)",
  };
  const std::vector<sql::Value> samples = {
      sql::Value(),           sql::Value(int64_t{0}), sql::Value(int64_t{1}),
      sql::Value(int64_t{2}), sql::Value(int64_t{3}), sql::Value(int64_t{5}),
      sql::Value(int64_t{7}), sql::Value(int64_t{9})};
  const sql::Identifier column("id");

  for (const auto &where : wheres) {
    const std::string sql = "SELECT * FROM users WHERE " + where;
    auto query = build_query(sql);
    CHECK(query.has_value());
    if (!query.has_value()) {
      continue;
    }
    const sql::Condition *original = query->select()->where.get();

    auto optimized = optimizer.optimize(*query);
    CHECK(optimized.has_value());
    if (!optimized.has_value()) {
      continue;
    }

    for (const auto &value : samples) {
      const bool expected = plantest::sql_keeps(original, column, value);
      const bool actual = plantest::plan_keeps(*optimized, column, value);
      if (expected != actual) {
        // 失败信息里带上谓词，方便直接定位
        CHECK_EQ(actual ? "kept" : "dropped", expected ? "kept" : "dropped");
        CHECK_EQ(where, std::string("<plan disagrees with predicate>"));
      }
    }
  }
}
