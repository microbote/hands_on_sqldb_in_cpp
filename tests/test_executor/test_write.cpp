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
    if (!found.has_value()) {
      return std::nullopt;
    }
    return std::move(*found);
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

TEST(Write, InsertMultiRowValuesInsertsEveryRow) {
  Fixture env;
  const size_t before = env.row_count();

  auto cursor = exectest::run_sql(
      *env.catalog,
      "INSERT INTO users (id, name, age) VALUES (100, 'a', 1), (101, 'b', 2)");
  CHECK(cursor != nullptr);
  if (cursor == nullptr) {
    return;
  }
  CHECK_EQ(cursor->affected_rows(), size_t{2});
  CHECK_EQ(env.row_count(), before + 2);
  const auto first = env.row(100);
  const auto second = env.row(101);
  CHECK(first.has_value());
  CHECK(second.has_value());
  if (first.has_value()) {
    CHECK_EQ((*first)[1].as_str(), std::string("a"));
  }
  if (second.has_value()) {
    CHECK_EQ((*second)[2].as_int(), int64_t{2});
  }
}

// 多行 VALUES 中途撞主键：执行层报错（整条语句的原子性由 session 的
// 自动提交守卫保证 —— 执行器这一层没有事务，见 test_session 的用例）
TEST(Write, InsertMultiRowValuesReportsConflict) {
  Fixture env;

  std::string error;
  auto cursor = exectest::run_sql(
      *env.catalog,
      "INSERT INTO users (id, name, age) VALUES (100, 'a', 1), (3, 'clash', 2)",
      &error);
  CHECK(cursor == nullptr);
  CHECK(error.find("duplicate primary key") != std::string::npos);
}

TEST(Write, InsertDuplicatePrimaryKeyFailsAndKeepsOldRow) {
  Fixture env;
  size_t before = env.row_count();

  std::string error;
  auto cursor = exectest::run_sql(
      *env.catalog, "INSERT INTO users (id, name, age) VALUES (3, 'clash', 7)",
      &error);
  CHECK(cursor == nullptr);
  CHECK(error.find("duplicate primary key") != std::string::npos);

  // 没有覆盖：原来的行还在，行数也没变（INSERT 不是 UPSERT）
  CHECK_EQ(env.row_count(), before);
  const auto row = env.row(3);
  CHECK(row.has_value());
  if (row.has_value()) {
    CHECK((*row)[1].as_str() != std::string("clash"));
  }

  // 换个没被占用的主键照样能插
  auto fresh = exectest::run_sql(
      *env.catalog, "INSERT INTO users (id, name, age) VALUES (1000, 'ok', 1)");
  CHECK(fresh != nullptr);
  if (fresh != nullptr) {
    CHECK_EQ(fresh->affected_rows(), size_t{1});
  }
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
