// tests/test_executor/test_write.cpp
//
// 写语句：Update / Delete / Insert（都在 open() 里把活干完，next() 只返回
// END）。
#include "test_framework.h"

#include <memory>
#include <string>
#include <vector>

#include "exec_test_util.h"

namespace {

struct Fixture {
  std::shared_ptr<kv::MockEngine> engine;
  std::unique_ptr<sql::KVCatalog> catalog;

  explicit Fixture(int rows = 9) {
    engine = exectest::open_engine();
    catalog = std::make_unique<sql::KVCatalog>(engine);
    CHECK(exectest::setup(*catalog));
    CHECK(exectest::seed_users(*catalog, rows));
  }

  std::optional<sql::Row> row(int64_t id) {
    auto table = catalog->open_table(sql::Identifier("testdb"),
                                     sql::Identifier("users"));
    if (!table.has_value()) {
      return std::nullopt;
    }
    auto found = table->find(sql::Value(id, sql::DataType::INT));
    if (!found.has_value() || !found->has_value()) {
      return std::nullopt;
    }
    return std::move(**found);
  }

  size_t row_count() {
    auto table = catalog->open_table(sql::Identifier("testdb"),
                                     sql::Identifier("users"));
    if (!table.has_value()) {
      return 0;
    }
    auto count = table->row_count();
    return count.has_value() ? *count : 0;
  }
};

} // namespace

// ============================================================
// UPDATE
// ============================================================
TEST(Write, UpdateByPkAffectsOneRow) {
  Fixture env;
  auto cursor =
      exectest::run_sql(*env.catalog, "UPDATE users SET age = 99 WHERE id = 3");
  CHECK(cursor != nullptr);
  if (cursor == nullptr) {
    return;
  }
  CHECK_EQ(cursor->affected_rows(), size_t{1});
  CHECK(!cursor->next().has_value()); // 写语句没有结果行

  const auto updated = env.row(3);
  CHECK(updated.has_value());
  if (updated.has_value()) {
    CHECK_EQ((*updated)[2].as_int(), int64_t{99});
    CHECK_EQ((*updated)[1].as_str(), std::string("user3")); // 其它列没动
  }
  const auto untouched = env.row(4);
  CHECK(untouched.has_value());
  if (untouched.has_value()) {
    CHECK_EQ((*untouched)[2].as_int(), int64_t{40});
  }
}

TEST(Write, UpdateWithoutWhereTouchesEveryRow) {
  Fixture env;
  auto cursor = exectest::run_sql(*env.catalog, "UPDATE users SET age = 1");
  CHECK(cursor != nullptr);
  if (cursor == nullptr) {
    return;
  }
  // 没有 WHERE = 全表更新（绝不能是"零行"）
  CHECK_EQ(cursor->affected_rows(), size_t{9});
  const auto row = env.row(7);
  CHECK(row.has_value());
  if (row.has_value()) {
    CHECK_EQ((*row)[2].as_int(), int64_t{1});
  }
}

TEST(Write, UpdateWithNonPrimaryKeyFilter) {
  Fixture env;
  auto cursor = exectest::run_sql(
      *env.catalog, "UPDATE users SET age = 7 WHERE name = 'user5'");
  CHECK(cursor != nullptr);
  if (cursor == nullptr) {
    return;
  }
  CHECK_EQ(cursor->affected_rows(), size_t{1});
  const auto row = env.row(5);
  CHECK(row.has_value());
  if (row.has_value()) {
    CHECK_EQ((*row)[2].as_int(), int64_t{7});
  }
}

TEST(Write, UpdateWithContradictoryWhereAffectsNothing) {
  Fixture env;
  auto cursor = exectest::run_sql(
      *env.catalog, "UPDATE users SET age = 1 WHERE id > 5 AND id < 3");
  CHECK(cursor != nullptr);
  if (cursor == nullptr) {
    return;
  }
  CHECK_EQ(cursor->affected_rows(), size_t{0});
  CHECK_EQ(env.row_count(), size_t{9});
}

TEST(Write, UpdateThatWouldNullOutNotNullColumnFails) {
  Fixture env;
  // name 是 NOT NULL：写 NULL 必须失败（SCHEMA_ERROR），且不落库
  auto cursor = exectest::run_sql(*env.catalog,
                                  "UPDATE users SET name = NULL WHERE id = 1");
  CHECK(cursor == nullptr);
}

// ============================================================
// DELETE
// ============================================================
TEST(Write, DeleteByRangeRemovesRows) {
  Fixture env;
  auto cursor = exectest::run_sql(
      *env.catalog, "DELETE FROM users WHERE id >= 3 AND id <= 5");
  CHECK(cursor != nullptr);
  if (cursor == nullptr) {
    return;
  }
  CHECK_EQ(cursor->affected_rows(), size_t{3});
  CHECK_EQ(env.row_count(), size_t{6});
  CHECK(!env.row(4).has_value());
  CHECK(env.row(6).has_value());
}

TEST(Write, DeleteInListRemovesExactRows) {
  Fixture env;
  auto cursor = exectest::run_sql(*env.catalog,
                                  "DELETE FROM users WHERE id IN (1, 3, 5)");
  CHECK(cursor != nullptr);
  if (cursor == nullptr) {
    return;
  }
  CHECK_EQ(cursor->affected_rows(), size_t{3});
  CHECK_EQ(env.row_count(), size_t{6});
  CHECK(!env.row(1).has_value());
  CHECK(env.row(2).has_value());
}

TEST(Write, DeleteWithoutWhereEmptiesTheTable) {
  Fixture env;
  auto cursor = exectest::run_sql(*env.catalog, "DELETE FROM users");
  CHECK(cursor != nullptr);
  if (cursor == nullptr) {
    return;
  }
  CHECK_EQ(cursor->affected_rows(), size_t{9});
  CHECK_EQ(env.row_count(), size_t{0});
}

// ============================================================
// INSERT
// ============================================================
TEST(Write, InsertSingleRowWithColumnList) {
  Fixture env;
  auto cursor = exectest::run_sql(
      *env.catalog,
      "INSERT INTO users (id, name, age) VALUES (100, 'new', 42)");
  CHECK(cursor != nullptr);
  if (cursor == nullptr) {
    return;
  }
  CHECK_EQ(cursor->affected_rows(), size_t{1});
  const auto inserted = env.row(100);
  CHECK(inserted.has_value());
  if (inserted.has_value()) {
    CHECK_EQ((*inserted)[1].as_str(), std::string("new"));
    CHECK_EQ((*inserted)[2].as_int(), int64_t{42});
  }
}

TEST(Write, InsertMultiRowValuesIsNotSupportedYet) {
  Fixture env;
  // 已知缺口：语法只能跟一行 VALUES（builder 里已经预留了多行处理），
  // 所以这里解析失败 -> 执行层拿不到计划。等 parser 放开多行后
  // 这条用例应该改成断言 affected_rows == 2。
  std::string error;
  auto cursor = exectest::run_sql(
      *env.catalog,
      "INSERT INTO users (id, name, age) VALUES (100, 'a', 1), (101, 'b', 2)",
      &error);
  CHECK(cursor == nullptr);
  CHECK(!error.empty());
}

TEST(Write, InsertWithoutColumnListUsesSchemaOrder) {
  Fixture env;
  auto cursor =
      exectest::run_sql(*env.catalog, "INSERT INTO users VALUES (100, 'a', 1)");
  CHECK(cursor != nullptr);
  if (cursor == nullptr) {
    return;
  }
  CHECK_EQ(cursor->affected_rows(), size_t{1});
  CHECK(env.row(100).has_value());
}

TEST(Write, InsertMissingNotNullColumnFails) {
  Fixture env;
  // 没给 name（NOT NULL）-> 失败
  auto cursor =
      exectest::run_sql(*env.catalog, "INSERT INTO users (id) VALUES (100)");
  CHECK(cursor == nullptr);
  CHECK(!env.row(100).has_value());
}

TEST(Write, InsertColumnCountMismatchFails) {
  Fixture env;
  auto cursor = exectest::run_sql(
      *env.catalog, "INSERT INTO users (id, name) VALUES (100, 'a', 1)");
  CHECK(cursor == nullptr);
}

TEST(Write, InsertNullPrimaryKeyFails) {
  Fixture env;
  auto cursor = exectest::run_sql(
      *env.catalog, "INSERT INTO users (id, name, age) VALUES (NULL, 'a', 1)");
  CHECK(cursor == nullptr);
}

TEST(Write, InsertThenQueryRoundTrip) {
  Fixture env;
  auto insert = exectest::run_sql(
      *env.catalog,
      "INSERT INTO users (id, name, age) VALUES (100, '新用户', 42)");
  CHECK(insert != nullptr);

  auto cursor = exectest::run_sql(
      *env.catalog, "SELECT id, name, age FROM users WHERE id = 100");
  CHECK(cursor != nullptr);
  if (cursor == nullptr) {
    return;
  }
  const auto rows = exectest::collect_rows(*cursor);
  CHECK_EQ(rows.size(), size_t{1});
  if (rows.size() == 1) {
    CHECK_EQ(rows[0][0].as_int(), int64_t{100});
    CHECK_EQ(rows[0][1].as_str(), std::string("新用户"));
    CHECK_EQ(rows[0][2].as_int(), int64_t{42});
  }
}

TEST(Write, WriteCursorHasNoResultRowsAndIsAffectedCounted) {
  Fixture env;
  auto cursor =
      exectest::run_sql(*env.catalog, "DELETE FROM users WHERE id = 1");
  CHECK(cursor != nullptr);
  if (cursor == nullptr) {
    return;
  }
  // 写语句在 execute() 里就执行完了：游标只会返回 END
  auto first = cursor->next();
  CHECK(!first.has_value());
  CHECK(first.error().end());
  CHECK_EQ(cursor->affected_rows(), size_t{1});
}
