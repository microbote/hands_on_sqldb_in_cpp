// tests/test_relation/test_table.cpp
//
// Table 视图：点查、区间扫描（正/反向）、写路径、行编解码与错误码。
#include "test_framework.h"

#include <memory>
#include <string>
#include <vector>

#include "relation/cursor.h"
#include "relation/table.h"
#include "relation_test_util.h"
#include "storage/mock_engine/mock_engine.h"

namespace {

// 建一张 users 表并塞入 1..9 行（age = id * 10）
struct Fixture {
  std::shared_ptr<kv::MockEngine> engine;
  std::unique_ptr<sql::Table> table;

  Fixture() {
    engine = reltest::open_engine();
    table = std::make_unique<sql::Table>(engine, sql::Identifier("shop"),
                                         reltest::make_users_schema());
    for (int64_t id = 1; id <= 9; ++id) {
      auto inserted = table->insert(
          reltest::users_row(id, "user" + std::to_string(id), id * 10));
      CHECK(inserted.has_value());
    }
  }

  std::vector<int64_t> scan_ids(const sql::KeyRange &range,
                                bool ascending = true) {
    std::vector<int64_t> ids;
    auto cursor = table->scan(range, ascending);
    while (true) {
      auto row = cursor->next();
      if (!row.has_value()) {
        CHECK(row.error().end()); // 扫到区间边界 = 正常结束，不是错误
        break;
      }
      ids.push_back((*row)[0].as_int());
    }
    return ids;
  }

  std::vector<int64_t> scan_all_ids(bool ascending = true) {
    return scan_ids(sql::KeyRange::all(sql::DataType::INT), ascending);
  }
};

const sql::Value int_value(int64_t v) {
  return sql::Value(v, sql::DataType::INT);
}

} // namespace

// ============================================================
// 点查
// ============================================================
TEST(Table, InsertThenGet) {
  Fixture f;
  auto row = f.table->get(int_value(3));
  CHECK(row.has_value());
  if (row.has_value()) {
    CHECK_EQ((*row)[0].as_int(), int64_t{3});
    CHECK_EQ((*row)[1].as_str(), std::string("user3"));
    CHECK_EQ((*row)[2].as_int(), int64_t{30});
  }
  CHECK_EQ(*f.table->row_count(), size_t{9});
}

TEST(Table, FindMissingIsNotAnError) {
  Fixture f;
  auto row = f.table->find(int_value(100));
  CHECK(row.has_value());
  if (row.has_value()) {
    CHECK(!row->has_value());
  }
  // get() 则把"不存在"当错误
  auto missing = f.table->get(int_value(100));
  CHECK(!missing.has_value());
  if (!missing.has_value()) {
    CHECK(missing.error().code == sql::RelErrorCode::NOT_FOUND);
  }
}

TEST(Table, GetWithNullKeyIsRejected) {
  Fixture f;
  auto row = f.table->get(sql::Value());
  CHECK(!row.has_value());
  if (!row.has_value()) {
    CHECK(row.error().code == sql::RelErrorCode::PRIMARY_KEY_NULL);
  }
}

// ============================================================
// 扫描
// ============================================================
TEST(Table, ScanAllFollowsPrimaryKeyOrder) {
  Fixture f;
  const auto ids = f.scan_all_ids();
  CHECK_EQ(ids.size(), size_t{9});
  for (size_t i = 0; i < ids.size(); ++i) {
    CHECK_EQ(ids[i], static_cast<int64_t>(i + 1));
  }
}

TEST(Table, ReverseScanFollowsPrimaryKeyDescending) {
  Fixture f;
  const auto ids = f.scan_all_ids(false);
  CHECK_EQ(ids.size(), size_t{9});
  if (!ids.empty()) {
    CHECK_EQ(ids.front(), int64_t{9});
    CHECK_EQ(ids.back(), int64_t{1});
  }
}

TEST(Table, RangeScanIsHalfOpen) {
  Fixture f;
  // [3, 7) -> 3,4,5,6
  const auto ids = f.scan_ids(sql::KeyRange::range(int_value(3), int_value(7)));
  CHECK_EQ(ids.size(), size_t{4});
  if (ids.size() == 4) {
    CHECK_EQ(ids.front(), int64_t{3});
    CHECK_EQ(ids.back(), int64_t{6});
  }
}

TEST(Table, PoinRangeScanReturnsOneRow) {
  Fixture f;
  const auto ids = f.scan_ids(sql::KeyRange::point(int_value(5)));
  CHECK_EQ(ids.size(), size_t{1});
  if (!ids.empty()) {
    CHECK_EQ(ids.front(), int64_t{5});
  }
}

TEST(Table, UnboundedRangeScanCoversWholeTable) {
  Fixture f;
  // (NULL, +∞)：排除 NULL 之后的全表
  const auto ids = f.scan_ids(sql::KeyRange::non_null(sql::DataType::INT));
  CHECK_EQ(ids.size(), size_t{9});
}

TEST(Table, EmptyRangeScansNothing) {
  Fixture f;
  const auto ids = f.scan_ids(sql::KeyRange::range(int_value(7), int_value(3)));
  CHECK(ids.empty());
}

TEST(Table, CorruptRowIsReportedAsError) {
  Fixture f;
  // 直接往 KV 里塞一段垃圾，游标必须报错而不是静默跳过
  const std::string key = f.table->encode_key(int_value(4));
  CHECK(f.engine->put(key, "not-a-row") == kv::Status::OK);

  auto cursor = f.table->scan_all();
  sql::CursorError last;
  while (true) {
    auto row = cursor->next();
    if (!row.has_value()) {
      last = row.error();
      break;
    }
  }
  CHECK(last.is_error());
  CHECK(last.code == sql::CursorErrorCode::SCHEMA_ERROR);
}

// ============================================================
// 游标契约（sql::Cursor）
// ============================================================
TEST(Cursor, EmptyScanEndsNormallyInsteadOfErroring) {
  Fixture f;
  auto cursor = f.table->scan(sql::KeyRange::range(int_value(7), int_value(3)));
  auto first = cursor->next();
  CHECK(!first.has_value());
  CHECK(first.error().end());       // END = 正常结束
  CHECK(!first.error().is_error()); // 不是错误
  // 再调用一次仍是 END（幂等），不会变成"错误"
  auto second = cursor->next();
  CHECK(!second.has_value());
  CHECK(second.error().end());
}

TEST(Cursor, ErrorIsStickyAcrossCalls) {
  Fixture f;
  const std::string key = f.table->encode_key(int_value(2));
  CHECK(f.engine->put(key, "garbage") == kv::Status::OK);

  auto cursor = f.table->scan_all();
  sql::CursorError saw;
  while (!saw.is_error()) {
    auto row = cursor->next();
    if (!row.has_value()) {
      saw = row.error();
    }
  }
  CHECK(saw.code == sql::CursorErrorCode::SCHEMA_ERROR);
  // 错误粘住：接着调用还是同一个错误，不会被误当成正常结束
  auto again = cursor->next();
  CHECK(!again.has_value());
  CHECK(again.error().code == sql::CursorErrorCode::SCHEMA_ERROR);
}

TEST(Cursor, CloseIsIdempotentAndEndsTheStream) {
  Fixture f;
  auto cursor = f.table->scan_all();
  CHECK(cursor->next().has_value());
  cursor->close();
  cursor->close(); // 幂等
  auto after = cursor->next();
  CHECK(!after.has_value());
  CHECK(after.error().end());
}

TEST(Cursor, ResetRescansTheSameRange) {
  Fixture f;
  auto cursor = f.table->scan(sql::KeyRange::range(int_value(2), int_value(5)));
  auto first = cursor->next();
  CHECK(first.has_value());
  if (first.has_value()) {
    CHECK_EQ((*first)[0].as_int(), int64_t{2});
  }
  cursor->reset();
  auto again = cursor->next();
  CHECK(again.has_value());
  if (again.has_value()) {
    CHECK_EQ((*again)[0].as_int(), int64_t{2}); // 回到区间起点
  }
  CHECK_EQ(cursor->range().to_string(), std::string("[2, 5)"));
}

// ============================================================
// 写入
// ============================================================
TEST(Table, RejectRowThatDoesNotMatchSchema) {
  Fixture f;
  // 列数不对
  sql::Row bad;
  bad.push_back(int_value(100));
  auto result = f.table->insert(bad);
  CHECK(!result.has_value());
  if (!result.has_value()) {
    CHECK(result.error().code == sql::RelErrorCode::SCHEMA_ERROR);
  }

  // NOT NULL 列写 NULL
  auto null_name = reltest::users_row(101, "x", 1);
  null_name[1] = sql::Value();
  auto null_result = f.table->insert(null_name);
  CHECK(!null_result.has_value());
}

TEST(Table, PartialUpdateChangesOnlyGivenColumns) {
  Fixture f;
  std::vector<std::pair<sql::Identifier, sql::Value>> assignments;
  assignments.emplace_back(sql::Identifier("age"), int_value(99));
  auto result = f.table->update(int_value(2), assignments);
  CHECK(result.has_value());

  auto row = f.table->get(int_value(2));
  CHECK(row.has_value());
  if (row.has_value()) {
    CHECK_EQ((*row)[0].as_int(), int64_t{2});
    CHECK_EQ((*row)[1].as_str(), std::string("user2")); // 没动
    CHECK_EQ((*row)[2].as_int(), int64_t{99});
  }
}

TEST(Table, UpdateWithMismatchedPrimaryKeyIsRejected) {
  Fixture f;
  auto result = f.table->update(int_value(3), reltest::users_row(4, "x", 1));
  CHECK(!result.has_value());
  if (!result.has_value()) {
    CHECK(result.error().code == sql::RelErrorCode::PRIMARY_KEY_MISMATCH);
  }
}

TEST(Table, UpdateUnknownColumnIsRejected) {
  Fixture f;
  std::vector<std::pair<sql::Identifier, sql::Value>> assignments;
  assignments.emplace_back(sql::Identifier("nope"), int_value(1));
  auto result = f.table->update(int_value(1), assignments);
  CHECK(!result.has_value());
  if (!result.has_value()) {
    CHECK(result.error().code == sql::RelErrorCode::COLUMN_NOT_FOUND);
  }
}

TEST(Table, RemoveAndTruncate) {
  Fixture f;
  CHECK(f.table->remove(int_value(1)).has_value());
  auto removed = f.table->find(int_value(1));
  CHECK(removed.has_value());
  if (removed.has_value()) {
    CHECK(!removed->has_value());
  }
  CHECK_EQ(*f.table->row_count(), size_t{8});

  CHECK(f.table->truncate().has_value());
  CHECK_EQ(*f.table->row_count(), size_t{0});
}

TEST(Table, StringPrimaryKeyRoundTrip) {
  auto engine = reltest::open_engine();
  sql::Table table(engine, sql::Identifier("shop"),
                   reltest::make_accounts_schema());

  for (const char *name : {"carol", "alice", "bob"}) {
    sql::Row row;
    row.push_back(sql::Value(name));
    row.push_back(sql::Value(int64_t{7}, sql::DataType::INT));
    CHECK(table.insert(row).has_value());
  }

  std::vector<std::string> names;
  auto cursor = table.scan_all();
  while (true) {
    auto row = cursor->next();
    if (!row.has_value()) {
      break;
    }
    names.push_back(row->operator[](0).as_str());
  }
  CHECK_EQ(names.size(), size_t{3});
  if (names.size() == 3) {
    CHECK_EQ(names[0], std::string("alice"));
    CHECK_EQ(names[1], std::string("bob"));
    CHECK_EQ(names[2], std::string("carol"));
  }
}
