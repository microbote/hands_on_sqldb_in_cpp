// tests/test_session/test_multi_insert.cpp
//
// 多行 VALUES + 主键冲突的**提前**检查。
//
// 原则：越早发现越好 —— 冲突在**校验期**（执行前）就报出来，事务不会
// 执行到一半才失败；报告位置指向那个具体的值，用户一眼能看出是哪一行。
#include "test_framework.h"

#include <string>

#include "session_test_util.h"

namespace {

size_t row_count(session::Session &session) {
  bool ok = false;
  const auto ids =
      sess_test::first_column_ints(session, "SELECT id FROM users", &ok);
  CHECK(ok);
  return ids.size();
}

} // namespace

TEST(MultiInsert, InsertsEveryRowAndReportsAffectedCount) {
  auto engine = sess_test::open_engine();
  session::Session session(engine);
  CHECK(sess_test::bootstrap(session, 0));

  session::SessionError error;
  auto cursor = sess_test::run(
      session,
      "INSERT INTO users (id, name, age) VALUES (1, 'a', 10), (2, 'b', 20), "
      "(3, 'c', 30)",
      &error);
  CHECK(cursor != nullptr);
  if (cursor != nullptr) {
    CHECK_EQ(cursor->affected_rows(), size_t{3});
  }

  bool ok = false;
  const auto ids = sess_test::first_column_ints(
      session, "SELECT id FROM users ORDER BY id", &ok);
  CHECK(ok);
  CHECK_EQ(ids.size(), size_t{3});
  CHECK_EQ(row_count(session), size_t{3});
}

TEST(MultiInsert, DuplicateInsideTheBatchIsRejectedBeforeExecution) {
  auto engine = sess_test::open_engine();
  session::Session session(engine);
  CHECK(sess_test::bootstrap(session, 0));

  session::SessionError error;
  auto cursor = sess_test::run(
      session,
      "INSERT INTO users (id, name, age) VALUES (1, 'a', 10), (2, 'b', 20), "
      "(1, 'again', 30)",
      &error);
  CHECK(cursor == nullptr);
  CHECK(error.code == session::SessionErrorCode::CONSTRAINT_VIOLATION);
  CHECK(error.message.find("duplicate primary key") != std::string::npos);
  // 报错位置指向**重复出现的那个值**（这一行的 id）
  CHECK(error.highlight(false).find("again") != std::string::npos);

  // 一行都没落：校验期就拒了，事务根本没开
  CHECK_EQ(row_count(session), size_t{0});
}

TEST(MultiInsert, DuplicateWithExistingRowIsRejectedBeforeExecution) {
  auto engine = sess_test::open_engine();
  session::Session session(engine);
  CHECK(sess_test::bootstrap(session, 2)); // 已有 id = 1, 2

  session::SessionError error;
  auto cursor = sess_test::run(
      session,
      "INSERT INTO users (id, name, age) VALUES (10, 'x', 1), (2, 'clash', 2)",
      &error);
  CHECK(cursor == nullptr);
  CHECK(error.code == session::SessionErrorCode::CONSTRAINT_VIOLATION);
  CHECK(error.message.find("already exists") != std::string::npos);
  // 报错位置指向那一行的 id 值
  CHECK(error.highlight(false).find("clash") != std::string::npos);

  // 前半截（id = 10）也没落下
  CHECK_EQ(row_count(session), size_t{2});
  bool ok = false;
  const auto ten = sess_test::first_column_ints(
      session, "SELECT id FROM users WHERE id = 10", &ok);
  CHECK(ok);
  CHECK_EQ(ten.size(), size_t{0});
}

TEST(MultiInsert, ConflictIsFoundBeforeTheTransactionStarts) {
  // 这是"提前校验"的真正价值：事务里的语句在校验期被拒，
  // 事务**不会**被标记中止（校验期错误不中止事务，见 readme 第 3 节），
  // 用户可以接着在同一个事务里干别的。
  auto engine = sess_test::open_engine();
  session::Session session(engine);
  CHECK(sess_test::bootstrap(session, 1)); // 已有 id = 1

  session::SessionError error;
  CHECK(sess_test::run(session, "BEGIN", &error) != nullptr);

  auto bad = sess_test::run(
      session, "INSERT INTO users (id, name, age) VALUES (1, 'clash', 1)",
      &error);
  CHECK(bad == nullptr);
  CHECK(error.code == session::SessionErrorCode::CONSTRAINT_VIOLATION);
  CHECK(session.in_transaction()); // 事务还活着

  // 同一个事务里继续干活，最后提交
  CHECK(sess_test::run(session,
                       "INSERT INTO users (id, name, age) VALUES (2, 'ok', 20)",
                       &error) != nullptr);
  CHECK(sess_test::run(session, "COMMIT", &error) != nullptr);
  CHECK_EQ(row_count(session), size_t{2});
}

TEST(MultiInsert, BatchConflictAlsoRespectsTheTransactionsOwnWrites) {
  // 事务里先插一行，再插同样的主键：第二条在**校验期**就该报
  // （探测走的是本连接的视图，能看到自己未提交的写）
  auto engine = sess_test::open_engine();
  session::Session session(engine);
  CHECK(sess_test::bootstrap(session, 0));

  session::SessionError error;
  CHECK(sess_test::run(session, "BEGIN", &error) != nullptr);
  CHECK(sess_test::run(session,
                       "INSERT INTO users (id, name, age) VALUES (1, 'a', 10)",
                       &error) != nullptr);

  auto again = sess_test::run(
      session, "INSERT INTO users (id, name, age) VALUES (1, 'b', 20)", &error);
  CHECK(again == nullptr);
  CHECK(error.code == session::SessionErrorCode::CONSTRAINT_VIOLATION);
  CHECK(session.in_transaction());

  CHECK(sess_test::run(session, "ROLLBACK", &error) != nullptr);
  CHECK_EQ(row_count(session), size_t{0});
}
