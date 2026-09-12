// tests/test_executor/test_sort_limit.cpp
//
// Sort / TopN / Limit：顺序、NULL 位置、tiebreaker、早停与内存上限。
#include "test_framework.h"

#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "exec_test_util.h"

namespace {

struct Fixture {
  std::shared_ptr<kv::MockEngine> engine;
  std::unique_ptr<sql::KVCatalog> catalog;

  explicit Fixture() {
    engine = exectest::open_engine();
    catalog = std::make_unique<sql::KVCatalog>(engine);
    CHECK(exectest::setup(*catalog));
    CHECK(exectest::seed_users(*catalog, 9)); // age = 10,20,...,90
  }

  std::shared_ptr<sql::Table> table() {
    auto opened = catalog->open_table(sql::Identifier("testdb"),
                                      sql::Identifier("users"));
    CHECK(opened.has_value());
    if (!opened.has_value()) {
      return nullptr;
    }
    return std::make_shared<sql::Table>(std::move(*opened));
  }
};

} // namespace

TEST(Sort, OrderByPrimaryKeyDescendingUsesReverseScan) {
  Fixture env;
  auto cursor =
      exectest::run_sql(*env.catalog, "SELECT * FROM users ORDER BY id DESC");
  CHECK(cursor != nullptr);
  if (cursor == nullptr) {
    return;
  }
  const auto ids = exectest::column_ints(*cursor);
  CHECK_EQ(ids.size(), size_t{9});
  if (!ids.empty()) {
    CHECK_EQ(ids.front(), int64_t{9});
    CHECK_EQ(ids.back(), int64_t{1});
  }
}

TEST(Sort, OrderByPrimaryKeyAscendingWithLimitStopsEarly) {
  Fixture env;
  // 塞一条行数据损坏的记录（主键最大）：只要执行器真的"取够 3 行就停"，
  // 就永远不会读到它，查询必须成功。这比数迭代次数更直接地验证早停。
  auto table = env.table();
  CHECK(table != nullptr);
  if (table != nullptr) {
    const std::string key =
        table->encode_key(sql::Value(int64_t{1000}, sql::DataType::INT));
    CHECK(env.engine->put(key, "corrupt-but-never-read") == kv::Status::OK);
  }

  auto cursor = exectest::run_sql(*env.catalog,
                                  "SELECT * FROM users ORDER BY id LIMIT 3");
  CHECK(cursor != nullptr);
  if (cursor == nullptr) {
    return;
  }
  sql::CursorError error;
  const auto ids = exectest::column_ints(*cursor, 0, &error);
  CHECK_EQ(ids.size(), size_t{3});
  CHECK(error.end());
  if (ids.size() == 3) {
    CHECK_EQ(ids[0], int64_t{1});
    CHECK_EQ(ids[2], int64_t{3});
  }

  // 同样的数据，不加 LIMIT 就会读到那条坏行 -> 必须报错（对照组）
  auto full = exectest::run_sql(*env.catalog, "SELECT * FROM users");
  CHECK(full != nullptr);
  if (full != nullptr) {
    sql::CursorError full_error;
    exectest::collect_rows(*full, &full_error);
    CHECK(full_error.is_error());
  }
}

TEST(Sort, OrderByNonPrimaryKeySortsRows) {
  Fixture env;
  auto cursor =
      exectest::run_sql(*env.catalog, "SELECT * FROM users ORDER BY age DESC");
  CHECK(cursor != nullptr);
  if (cursor == nullptr) {
    return;
  }
  const auto ids = exectest::column_ints(*cursor);
  CHECK_EQ(ids.size(), size_t{9});
  if (!ids.empty()) {
    CHECK_EQ(ids.front(), int64_t{9}); // age = 90
    CHECK_EQ(ids.back(), int64_t{1});  // age = 10
  }
}

TEST(Sort, TopNReturnsOnlyRequestedRows) {
  Fixture env;
  auto cursor = exectest::run_sql(
      *env.catalog, "SELECT * FROM users ORDER BY age DESC LIMIT 3");
  CHECK(cursor != nullptr);
  if (cursor == nullptr) {
    return;
  }
  const auto ids = exectest::column_ints(*cursor);
  CHECK_EQ(ids.size(), size_t{3});
  if (ids.size() == 3) {
    CHECK_EQ(ids[0], int64_t{9});
    CHECK_EQ(ids[1], int64_t{8});
    CHECK_EQ(ids[2], int64_t{7});
  }
}

TEST(Sort, TopNWithOffsetSkipsRows) {
  Fixture env;
  auto cursor = exectest::run_sql(
      *env.catalog, "SELECT * FROM users ORDER BY age DESC LIMIT 2 OFFSET 1");
  CHECK(cursor != nullptr);
  if (cursor == nullptr) {
    return;
  }
  const auto ids = exectest::column_ints(*cursor);
  CHECK_EQ(ids.size(), size_t{2});
  if (ids.size() == 2) {
    CHECK_EQ(ids[0], int64_t{8});
    CHECK_EQ(ids[1], int64_t{7});
  }
}

TEST(Sort, LimitWithoutOrderByTakesFirstRowsInScanOrder) {
  Fixture env;
  auto cursor = exectest::run_sql(*env.catalog, "SELECT * FROM users LIMIT 3");
  CHECK(cursor != nullptr);
  if (cursor == nullptr) {
    return;
  }
  const auto ids = exectest::column_ints(*cursor);
  CHECK_EQ(ids.size(), size_t{3});
  if (ids.size() == 3) {
    CHECK_EQ(ids[0], int64_t{1});
    CHECK_EQ(ids[2], int64_t{3});
  }
}

TEST(Sort, LimitZeroReturnsNothing) {
  Fixture env;
  auto cursor = exectest::run_sql(*env.catalog,
                                  "SELECT * FROM users ORDER BY age LIMIT 0");
  CHECK(cursor != nullptr);
  if (cursor == nullptr) {
    return;
  }
  sql::CursorError error;
  const auto rows = exectest::collect_rows(*cursor, &error);
  CHECK(rows.empty());
  CHECK(error.end());
}

TEST(Sort, NullsComeFirstInAscendingOrder) {
  auto engine = exectest::open_engine();
  sql::KVCatalog catalog(engine);
  CHECK(exectest::setup(catalog));
  // 三行：age = NULL / 20 / 10
  auto table =
      catalog.open_table(sql::Identifier("testdb"), sql::Identifier("users"));
  CHECK(table.has_value());
  if (table.has_value()) {
    CHECK(table->insert(exectest::users_row(1, "a", 20)).has_value());
    sql::Row with_null = exectest::users_row(2, "b", 0);
    with_null[2] = sql::Value(); // age = NULL
    CHECK(table->insert(with_null).has_value());
    CHECK(table->insert(exectest::users_row(3, "c", 10)).has_value());
  }

  auto cursor =
      exectest::run_sql(catalog, "SELECT * FROM users ORDER BY age ASC");
  CHECK(cursor != nullptr);
  if (cursor == nullptr) {
    return;
  }
  const auto rows = exectest::collect_rows(*cursor);
  CHECK_EQ(rows.size(), size_t{3});
  if (rows.size() == 3) {
    CHECK(rows[0][2].is_null()); // NULL 最小（MySQL：ASC 时在最前）
    CHECK_EQ(rows[1][2].as_int(), int64_t{10});
    CHECK_EQ(rows[2][2].as_int(), int64_t{20});
  }
}

TEST(Sort, TiesAreBrokenByPrimaryKey) {
  auto engine = exectest::open_engine();
  sql::KVCatalog catalog(engine);
  CHECK(exectest::setup(catalog));
  auto table =
      catalog.open_table(sql::Identifier("testdb"), sql::Identifier("users"));
  CHECK(table.has_value());
  if (table.has_value()) {
    // age 全相同：顺序由主键 tiebreaker 决定（升序），而不是扫描顺序的偶然结果
    CHECK(table->insert(exectest::users_row(3, "c", 50)).has_value());
    CHECK(table->insert(exectest::users_row(1, "a", 50)).has_value());
    CHECK(table->insert(exectest::users_row(2, "b", 50)).has_value());
  }

  auto cursor =
      exectest::run_sql(catalog, "SELECT * FROM users ORDER BY age LIMIT 2");
  CHECK(cursor != nullptr);
  if (cursor == nullptr) {
    return;
  }
  const auto ids = exectest::column_ints(*cursor);
  CHECK_EQ(ids.size(), size_t{2});
  if (ids.size() == 2) {
    CHECK_EQ(ids[0], int64_t{1});
    CHECK_EQ(ids[1], int64_t{2});
  }
}

TEST(Sort, FullSortBeyondMemoryLimitReportsError) {
  Fixture env;
  // 直接搭算子：全量排序 + 很小的行上限 -> MEMORY_LIMIT
  auto table = env.table();
  CHECK(table != nullptr);
  if (table == nullptr) {
    return;
  }
  plan::TableRef target;
  target.db = sql::Identifier("testdb");
  target.table = sql::Identifier("users");
  target.primary_key = sql::Identifier("id");
  target.primary_key_type = sql::DataType::INT;

  auto scan = std::make_unique<plan::FullScan>(target, true);
  auto scan_executor = std::make_unique<exec::ScanExecutor>(scan.get(), table);
  std::vector<sql::OrderByItem> order_by{
      sql::OrderByItem(sql::Identifier("age"), sql::OrderDirection::ASC)};
  plan::SortPlan sort_plan(nullptr, order_by, /*is_top_n=*/false, 0);
  exec::SortExecutor sorter(&sort_plan, std::move(scan_executor), table,
                            /*row_limit=*/3);
  const exec::ExecError status = sorter.open();
  CHECK(!status.ok());
  CHECK(status.code == exec::ExecErrorCode::MEMORY_LIMIT);
}
