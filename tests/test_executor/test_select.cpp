// tests/test_executor/test_select.cpp
//
// Scan / Filter / Project 的端到端行为（含 NULL 三值逻辑与错误传播）。
#include "test_framework.h"

#include <memory>
#include <string>
#include <vector>

#include "exec_test_util.h"

namespace {

// 一个装好 9 行数据的 catalog（id = 1..9，age = id*10）
struct Fixture {
  std::shared_ptr<kv::MockEngine> engine;
  std::unique_ptr<sql::KVCatalog> catalog;

  explicit Fixture(int rows = 9) {
    engine = exectest::open_engine();
    catalog = std::make_unique<sql::KVCatalog>(engine);
    CHECK(exectest::setup(*catalog));
    CHECK(exectest::seed_users(*catalog, rows));
  }
};

} // namespace

TEST(Select, ScansWholeTableInPrimaryKeyOrder) {
  Fixture env;
  auto cursor = exectest::run_sql(*env.catalog, "SELECT * FROM users");
  CHECK(cursor != nullptr);
  if (cursor == nullptr) {
    return;
  }
  const auto ids = exectest::column_ints(*cursor);
  CHECK_EQ(ids.size(), size_t{9});
  for (size_t i = 0; i < ids.size(); ++i) {
    CHECK_EQ(ids[i], static_cast<int64_t>(i + 1));
  }
}

TEST(Select, PkRangeNarrowsTheScan) {
  Fixture env;
  auto cursor = exectest::run_sql(
      *env.catalog, "SELECT * FROM users WHERE id >= 3 AND id < 7");
  CHECK(cursor != nullptr);
  if (cursor == nullptr) {
    return;
  }
  const auto ids = exectest::column_ints(*cursor);
  CHECK_EQ(ids.size(), size_t{4});
  if (ids.size() == 4) {
    CHECK_EQ(ids.front(), int64_t{3});
    CHECK_EQ(ids.back(), int64_t{6});
  }
}

TEST(Select, PointQueryReturnsSingleRow) {
  Fixture env;
  auto cursor =
      exectest::run_sql(*env.catalog, "SELECT * FROM users WHERE id = 5");
  CHECK(cursor != nullptr);
  if (cursor == nullptr) {
    return;
  }
  const auto ids = exectest::column_ints(*cursor);
  CHECK_EQ(ids.size(), size_t{1});
  if (!ids.empty()) {
    CHECK_EQ(ids.front(), int64_t{5});
  }
}

TEST(Select, InListBecomesOrderedRangeUnion) {
  Fixture env;
  // 故意乱序给出：输出必须仍是主键升序（全局一致的顺序）
  auto cursor = exectest::run_sql(*env.catalog,
                                  "SELECT * FROM users WHERE id IN (5, 1, 3)");
  CHECK(cursor != nullptr);
  if (cursor == nullptr) {
    return;
  }
  const auto ids = exectest::column_ints(*cursor);
  CHECK_EQ(ids.size(), size_t{3});
  if (ids.size() == 3) {
    CHECK_EQ(ids[0], int64_t{1});
    CHECK_EQ(ids[1], int64_t{3});
    CHECK_EQ(ids[2], int64_t{5});
  }
}

TEST(Select, NotEqualSkipsThePoint) {
  Fixture env;
  auto cursor =
      exectest::run_sql(*env.catalog, "SELECT * FROM users WHERE id != 5");
  CHECK(cursor != nullptr);
  if (cursor == nullptr) {
    return;
  }
  const auto ids = exectest::column_ints(*cursor);
  CHECK_EQ(ids.size(), size_t{8});
  for (const int64_t id : ids) {
    CHECK(id != 5);
  }
}

TEST(Select, NotInSkipsSeveralPoints) {
  Fixture env;
  auto cursor = exectest::run_sql(
      *env.catalog, "SELECT * FROM users WHERE id NOT IN (1, 2, 3)");
  CHECK(cursor != nullptr);
  if (cursor == nullptr) {
    return;
  }
  const auto ids = exectest::column_ints(*cursor);
  CHECK_EQ(ids.size(), size_t{6});
  if (!ids.empty()) {
    CHECK_EQ(ids.front(), int64_t{4});
  }
}

TEST(Select, NonPrimaryKeyFilterIsAppliedPerRow) {
  Fixture env;
  auto cursor = exectest::run_sql(*env.catalog,
                                  "SELECT * FROM users WHERE name = 'user3'");
  CHECK(cursor != nullptr);
  if (cursor == nullptr) {
    return;
  }
  const auto ids = exectest::column_ints(*cursor);
  CHECK_EQ(ids.size(), size_t{1});
  if (!ids.empty()) {
    CHECK_EQ(ids.front(), int64_t{3});
  }
}

TEST(Select, PkRangeAndFilterCombine) {
  Fixture env;
  auto cursor = exectest::run_sql(
      *env.catalog, "SELECT * FROM users WHERE id >= 2 AND age < 50");
  CHECK(cursor != nullptr);
  if (cursor == nullptr) {
    return;
  }
  const auto ids = exectest::column_ints(*cursor);
  // age = id*10 -> id 2,3,4 满足 age < 50
  CHECK_EQ(ids.size(), size_t{3});
  if (ids.size() == 3) {
    CHECK_EQ(ids[0], int64_t{2});
    CHECK_EQ(ids[2], int64_t{4});
  }
}

TEST(Select, ContradictoryConditionReturnsNoRows) {
  Fixture env;
  auto cursor = exectest::run_sql(
      *env.catalog, "SELECT * FROM users WHERE id > 5 AND id < 3");
  CHECK(cursor != nullptr);
  if (cursor == nullptr) {
    return;
  }
  sql::CursorError error;
  const auto rows = exectest::collect_rows(*cursor, &error);
  CHECK(rows.empty());
  CHECK(error.end()); // 正常结束，不是错误
}

TEST(Select, UnknownColumnMakesPredicateUnknown) {
  Fixture env;
  // 未知列 -> UNKNOWN -> WHERE 不保留任何行（三值逻辑）
  auto cursor =
      exectest::run_sql(*env.catalog, "SELECT * FROM users WHERE nope = 1");
  CHECK(cursor != nullptr);
  if (cursor == nullptr) {
    return;
  }
  sql::CursorError error;
  const auto rows = exectest::collect_rows(*cursor, &error);
  CHECK(rows.empty());
  CHECK(error.end());
}

TEST(Select, ProjectionKeepsOnlyListedColumns) {
  Fixture env;
  auto cursor = exectest::run_sql(*env.catalog,
                                  "SELECT id, name FROM users WHERE id = 2");
  CHECK(cursor != nullptr);
  if (cursor == nullptr) {
    return;
  }
  const auto rows = exectest::collect_rows(*cursor);
  CHECK_EQ(rows.size(), size_t{1});
  if (rows.size() == 1) {
    CHECK_EQ(rows[0].size(), size_t{2});
    CHECK_EQ(rows[0][0].as_int(), int64_t{2});
    CHECK_EQ(rows[0][1].as_str(), std::string("user2"));
  }
}

TEST(Select, SelectStarKeepsAllColumns) {
  Fixture env;
  auto cursor =
      exectest::run_sql(*env.catalog, "SELECT * FROM users WHERE id = 1");
  CHECK(cursor != nullptr);
  if (cursor == nullptr) {
    return;
  }
  const auto rows = exectest::collect_rows(*cursor);
  CHECK_EQ(rows.size(), size_t{1});
  if (rows.size() == 1) {
    CHECK_EQ(rows[0].size(), size_t{3});
  }
}

TEST(Select, CursorCloseIsIdempotentAndEndsStream) {
  Fixture env;
  auto cursor = exectest::run_sql(*env.catalog, "SELECT * FROM users");
  CHECK(cursor != nullptr);
  if (cursor == nullptr) {
    return;
  }
  CHECK(cursor->next().has_value());
  cursor->close();
  cursor->close(); // 幂等
  auto after = cursor->next();
  CHECK(!after.has_value());
  CHECK(after.error().end());
}

TEST(Select, CorruptRowReportsSchemaError) {
  Fixture env;
  // 往表里塞一段垃圾（key 用真实主键的编码）：
  // 执行器必须报 SCHEMA_ERROR，而不是静默跳过或崩溃
  auto table = env.catalog->open_table(sql::Identifier("testdb"),
                                       sql::Identifier("users"));
  CHECK(table.has_value());
  if (table.has_value()) {
    const std::string key =
        table->encode_key(sql::Value(int64_t{3}, sql::DataType::INT));
    CHECK(env.engine->put(key, "not-a-row") == kv::Status::OK);
  }

  auto cursor = exectest::run_sql(*env.catalog, "SELECT * FROM users");
  CHECK(cursor != nullptr);
  if (cursor == nullptr) {
    return;
  }
  sql::CursorError error;
  exectest::collect_rows(*cursor, &error);
  CHECK(error.is_error());
  CHECK(error.code == sql::CursorErrorCode::SCHEMA_ERROR);
}
