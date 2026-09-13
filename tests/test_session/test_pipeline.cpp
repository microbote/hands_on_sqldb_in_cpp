// tests/test_session/test_pipeline.cpp
//
// 端到端：一条 SQL 文本走完整链路（parser -> builder -> validator ->
// rewriter -> optimizer -> planner -> executor -> cursor）。
#include "test_framework.h"

#include <memory>
#include <string>
#include <vector>

#include "session_test_util.h"

TEST(Session, BootstrapCreatesDatabaseTableAndRows) {
  auto engine = sess_test::open_engine();
  session::Session session(engine);
  CHECK(sess_test::bootstrap(session, 3));
  CHECK(session.current_database() == sql::Identifier("shop"));

  bool ok = false;
  const auto ids =
      sess_test::first_column_ints(session, "SELECT id FROM users", &ok);
  CHECK(ok);
  CHECK_EQ(ids.size(), size_t{3});
  if (ids.size() == 3) {
    CHECK_EQ(ids[0], int64_t{1});
    CHECK_EQ(ids[2], int64_t{3});
  }
}

TEST(Session, SelectWithWhereOrderByLimit) {
  auto engine = sess_test::open_engine();
  session::Session session(engine);
  CHECK(sess_test::bootstrap(session, 9));

  bool ok = false;
  auto ids = sess_test::first_column_ints(
      session, "SELECT id FROM users WHERE id >= 3 ORDER BY id DESC LIMIT 3",
      &ok);
  CHECK(ok);
  CHECK_EQ(ids.size(), size_t{3});
  if (ids.size() == 3) {
    CHECK_EQ(ids[0], int64_t{9});
    CHECK_EQ(ids[1], int64_t{8});
    CHECK_EQ(ids[2], int64_t{7});
  }

  ids = sess_test::first_column_ints(session,
                                     "SELECT id FROM users WHERE id = 5", &ok);
  CHECK(ok);
  CHECK_EQ(ids.size(), size_t{1});

  ids = sess_test::first_column_ints(
      session, "SELECT id FROM users WHERE id IN (2, 4, 6)", &ok);
  CHECK(ok);
  CHECK_EQ(ids.size(), size_t{3});
}

TEST(Session, InsertUpdateDeleteAreVisibleToLaterSelects) {
  auto engine = sess_test::open_engine();
  session::Session session(engine);
  CHECK(sess_test::bootstrap(session, 3));

  // INSERT
  session::SessionError error;
  auto inserted = sess_test::run(
      session, "INSERT INTO users (id, name, age) VALUES (10, 'x', 100)",
      &error);
  CHECK(inserted != nullptr);
  if (inserted != nullptr) {
    CHECK_EQ(inserted->affected_rows(), size_t{1});
  }

  // UPDATE
  auto updated =
      sess_test::run(session, "UPDATE users SET age = 7 WHERE id = 2", &error);
  CHECK(updated != nullptr);
  if (updated != nullptr) {
    CHECK_EQ(updated->affected_rows(), size_t{1});
  }

  // DELETE
  auto deleted =
      sess_test::run(session, "DELETE FROM users WHERE id >= 3", &error);
  CHECK(deleted != nullptr);
  if (deleted != nullptr) {
    CHECK_EQ(deleted->affected_rows(), size_t{2}); // 3 和 10
  }

  bool ok = false;
  const auto rows = sess_test::first_column_ints(
      session, "SELECT id FROM users ORDER BY id", &ok);
  CHECK(ok);
  CHECK_EQ(rows.size(), size_t{2});
  if (rows.size() == 2) {
    CHECK_EQ(rows[0], int64_t{1});
    CHECK_EQ(rows[1], int64_t{2});
  }

  // 更新后的列值确实变了
  auto cursor = sess_test::run(session, "SELECT age FROM users WHERE id = 2");
  CHECK(cursor != nullptr);
  if (cursor != nullptr) {
    const auto values = sess_test::collect(*cursor);
    CHECK_EQ(values.size(), size_t{1});
    if (values.size() == 1) {
      CHECK_EQ(values[0][0].as_int(), int64_t{7});
    }
  }
}

TEST(Session, DropTableRemovesDataAndTable) {
  auto engine = sess_test::open_engine();
  session::Session session(engine);
  CHECK(sess_test::bootstrap(session, 2));

  session::SessionError error;
  CHECK(sess_test::run(session, "DROP TABLE users", &error) != nullptr);
  CHECK(!session.catalog().table_exists(sql::Identifier("shop"),
                                        sql::Identifier("users")));

  // 重新建同名表：数据必须是干净的
  CHECK(sess_test::run(session,
                       "CREATE TABLE users (id INT PRIMARY KEY, name "
                       "VARCHAR(32) NOT NULL, age INT)",
                       &error) != nullptr);
  bool ok = false;
  const auto ids =
      sess_test::first_column_ints(session, "SELECT id FROM users", &ok);
  CHECK(ok);
  CHECK(ids.empty());
}

TEST(Session, DropThenCreateDatabase) {
  auto engine = sess_test::open_engine();
  session::Session session(engine);
  CHECK(sess_test::bootstrap(session, 1));

  session::SessionError error;
  CHECK(sess_test::run(session, "CREATE DATABASE other", &error) != nullptr);
  CHECK(sess_test::run(session, "DROP DATABASE other", &error) != nullptr);
  CHECK(!session.catalog().database_exists(sql::Identifier("other")));

  // 删掉当前库：后续语句没有 current database，必须报错而不是瞎查
  CHECK(sess_test::run(session, "DROP DATABASE shop", &error) != nullptr);
  CHECK(session.current_database().empty());
  CHECK(sess_test::run(session, "SELECT * FROM users", &error) == nullptr);
}

TEST(Session, UseSwitchesCurrentDatabase) {
  auto engine = sess_test::open_engine();
  session::Session session(engine);
  CHECK(sess_test::bootstrap(session, 1));

  session::SessionError error;
  CHECK(sess_test::run(session, "CREATE DATABASE db2", &error) != nullptr);
  CHECK(sess_test::run(session, "USE db2", &error) != nullptr);
  CHECK(session.current_database() == sql::Identifier("db2"));

  // 新库里没有 users
  CHECK(sess_test::run(session, "SELECT * FROM users", &error) == nullptr);
  // 切回去就能查
  CHECK(sess_test::run(session, "USE shop", &error) != nullptr);
  CHECK(sess_test::run(session, "SELECT * FROM users", &error) != nullptr);
}

TEST(Session, SqlWithOrWithoutSemicolonBothWork) {
  auto engine = sess_test::open_engine();
  session::Session session(engine);
  CHECK(sess_test::bootstrap(session, 2));

  session::SessionError error;
  CHECK(sess_test::run(session, "SELECT id FROM users", &error) != nullptr);
  CHECK(sess_test::run(session, "SELECT id FROM users;", &error) != nullptr);
  // 结尾空白也要能处理
  CHECK(sess_test::run(session, "SELECT id FROM users;   ", &error) != nullptr);
}

TEST(Session, WriteCursorHasNoRowsAndReportsAffectedCount) {
  auto engine = sess_test::open_engine();
  session::Session session(engine);
  CHECK(sess_test::bootstrap(session, 3));

  session::SessionError error;
  auto cursor =
      sess_test::run(session, "DELETE FROM users WHERE id = 1", &error);
  CHECK(cursor != nullptr);
  if (cursor == nullptr) {
    return;
  }
  CHECK_EQ(cursor->affected_rows(), size_t{1});
  auto row = cursor->next();
  CHECK(!row.has_value());
  CHECK(row.error().end()); // 写语句没有结果行
}

TEST(Session, DdlReturnsEmptyCursor) {
  auto engine = sess_test::open_engine();
  session::Session session(engine);

  session::SessionError error;
  auto cursor = sess_test::run(session, "CREATE DATABASE shop", &error);
  CHECK(cursor != nullptr);
  if (cursor == nullptr) {
    return;
  }
  CHECK_EQ(cursor->affected_rows(), size_t{0});
  auto row = cursor->next();
  CHECK(!row.has_value());
  CHECK(row.error().end());
}

TEST(Session, CorruptRowSurfacesOnSelect) {
  auto engine = sess_test::open_engine();
  session::Session session(engine);
  CHECK(sess_test::bootstrap(session, 2));

  // 把一条行数据写坏（绕过 SQL 层，直接落 KV）
  auto table = session.catalog().open_table(sql::Identifier("shop"),
                                            sql::Identifier("users"));
  CHECK(table.has_value());
  if (table.has_value()) {
    const std::string key =
        table->encode_key(sql::Value(int64_t{2}, sql::DataType::INT));
    CHECK(engine->put(key, "garbage") == kv::Status::OK);
  }

  session::SessionError error;
  auto cursor = sess_test::run(session, "SELECT * FROM users", &error);
  CHECK(cursor != nullptr); // 惰性执行：这里还不会发现
  if (cursor == nullptr) {
    return;
  }
  sql::CursorError cursor_error;
  sess_test::collect(*cursor, &cursor_error);
  CHECK(cursor_error.is_error());
  CHECK(cursor_error.code == sql::CursorErrorCode::SCHEMA_ERROR);
}
