// tests/test_session/test_transaction.cpp
//
// 语句级事务（自动提交）：session 把每条写语句/DDL 包成一个事务 ——
// 中途失败不留半截写，成功则一次原子提交。
#include "test_framework.h"

#include <memory>
#include <string>

#include "session_test_util.h"

TEST(TxSession, FailedStatementLeavesNoPartialWrites) {
  auto engine = sess_test::open_engine();
  session::Session session(engine);
  CHECK(sess_test::bootstrap(session, 2));

  // 模拟写入失败（提交时失败）：语句必须整体失败，且一行都不落
  engine->set_fail_writes(true);
  session::SessionError error;
  auto failed = sess_test::run(
      session, "INSERT INTO users (id, name, age) VALUES (10, 'x', 1)", &error);
  CHECK(failed == nullptr);
  CHECK(!error.ok());
  engine->set_fail_writes(false);

  bool ok = false;
  const auto ids = sess_test::first_column_ints(
      session, "SELECT id FROM users ORDER BY id", &ok);
  CHECK(ok);
  CHECK_EQ(ids.size(), size_t{2}); // 还是原来的两行

  // 失败之后事务被回滚，引擎/会话仍然可用
  auto retry = sess_test::run(
      session, "INSERT INTO users (id, name, age) VALUES (10, 'x', 1)", &error);
  CHECK(retry != nullptr);
  if (retry != nullptr) {
    CHECK_EQ(retry->affected_rows(), size_t{1});
  }
}

TEST(TxSession, DdlIsAtomicSoNoHalfCreatedTable) {
  auto engine = sess_test::open_engine();
  session::Session session(engine);
  CHECK(sess_test::bootstrap(session, 1));

  engine->set_fail_writes(true);
  session::SessionError error;
  auto failed = sess_test::run(
      session, "CREATE TABLE t2 (id INT PRIMARY KEY, v INT)", &error);
  CHECK(failed == nullptr);
  engine->set_fail_writes(false);

  // 关键点：建表要写 schema + 表名单 + 统计三条 key（还有数据 key）。
  // 没有事务时中途失败会留下"schema 里有、名单里没有"的半成品；
  // 有事务（缓冲后一次提交）就必须什么都不留。
  CHECK(!session.catalog().table_exists(sql::Identifier("shop"),
                                        sql::Identifier("t2")));
  CHECK(!session.catalog()
             .get_table_schema(sql::Identifier("shop"), sql::Identifier("t2"))
             .has_value());

  // 重试成功
  CHECK(sess_test::run(session, "CREATE TABLE t2 (id INT PRIMARY KEY, v INT)",
                       &error) != nullptr);
  CHECK(session.catalog().table_exists(sql::Identifier("shop"),
                                       sql::Identifier("t2")));
}

TEST(TxSession, SuccessfulStatementIsVisibleAfterCommit) {
  auto engine = sess_test::open_engine();
  session::Session session(engine);
  CHECK(sess_test::bootstrap(session, 2));

  session::SessionError error;
  auto inserted = sess_test::run(
      session, "INSERT INTO users (id, name, age) VALUES (10, 'x', 1)", &error);
  CHECK(inserted != nullptr);
  // 提交之后新数据对"新语句"可见（这里由同一会话的下一条 SELECT 验证）
  bool ok = false;
  const auto ids =
      sess_test::first_column_ints(session, "SELECT id FROM users", &ok);
  CHECK(ok);
  CHECK_EQ(ids.size(), size_t{3});

  // 元数据（行数统计）也在同一个事务里，提交后一致
  auto stats = session.catalog().table_stats(sql::Identifier("shop"),
                                             sql::Identifier("users"));
  CHECK(stats.has_value());
  if (stats.has_value()) {
    CHECK_EQ(stats->row_count, int64_t{3});
  }
}

TEST(TxSession, ReadStatementDoesNotHoldTheWriteLock) {
  auto engine = sess_test::open_engine();
  session::Session session(engine);
  CHECK(sess_test::bootstrap(session, 2));

  // SELECT 不包事务：执行完之后引擎不应该还停在事务里
  bool ok = false;
  sess_test::first_column_ints(session, "SELECT id FROM users", &ok);
  CHECK(ok);
  CHECK(!engine->in_transaction());
  CHECK(engine->begin_transaction() == kv::Status::OK); // 写锁是空着的
  CHECK(engine->rollback_transaction() == kv::Status::OK);
}

// ============================================================
// 显式多语句事务：BEGIN / COMMIT / ROLLBACK
// ============================================================
TEST(TxSession, BeginSeesOwnWritesAndCommitPersists) {
  auto engine = sess_test::open_engine();
  session::Session session(engine);
  CHECK(sess_test::bootstrap(session, 0));

  session::SessionError error;
  CHECK(sess_test::run(session, "BEGIN", &error) != nullptr);
  CHECK(session.in_transaction());
  // 插三行：既验证"事务内看到自己的写"，也回归 Table::scan 里
  // 临时 KeyRange 的生命周期问题（曾经只扫出第一行）
  for (int id = 1; id <= 3; ++id) {
    CHECK(sess_test::run(session,
                         "INSERT INTO users (id, name, age) VALUES (" +
                             std::to_string(id) + ", 'u" + std::to_string(id) +
                             "', " + std::to_string(id * 10) + ")",
                         &error) != nullptr);
  }
  // **事务内的 SELECT 能看到自己未提交的插入**（靠合并迭代器）
  bool ok = false;
  auto ids = sess_test::first_column_ints(session, "SELECT id FROM users", &ok);
  CHECK(ok);
  CHECK_EQ(ids.size(), size_t{3});
  if (ids.size() == 3) {
    CHECK_EQ(ids[0], int64_t{1});
    CHECK_EQ(ids[1], int64_t{2});
    CHECK_EQ(ids[2], int64_t{3});
  }

  CHECK(sess_test::run(session, "COMMIT", &error) != nullptr);
  CHECK(!session.in_transaction());
  CHECK(!engine->in_transaction());
  // 提交之后新语句照样能看到
  ids = sess_test::first_column_ints(session, "SELECT id FROM users", &ok);
  CHECK(ok);
  CHECK_EQ(ids.size(), size_t{3});
}

TEST(TxSession, RollbackDiscardsTheWholeTransaction) {
  auto engine = sess_test::open_engine();
  session::Session session(engine);
  CHECK(sess_test::bootstrap(session, 2)); // 已有 1、2 两行

  session::SessionError error;
  CHECK(sess_test::run(session, "BEGIN", &error) != nullptr);
  CHECK(sess_test::run(session,
                       "INSERT INTO users (id, name, age) VALUES (10, 'x', 1)",
                       &error) != nullptr);
  CHECK(sess_test::run(session, "DELETE FROM users WHERE id = 1", &error) !=
        nullptr);
  CHECK(sess_test::run(session, "UPDATE users SET age = 99 WHERE id = 2",
                       &error) != nullptr);
  CHECK(sess_test::run(session, "ROLLBACK", &error) != nullptr);
  CHECK(!session.in_transaction());

  // 三种改动全部撤销：插入没了、删除恢复、更新恢复
  bool ok = false;
  const auto ids = sess_test::first_column_ints(
      session, "SELECT id FROM users ORDER BY id", &ok);
  CHECK(ok);
  CHECK_EQ(ids.size(), size_t{2});
  auto cursor = sess_test::run(session, "SELECT age FROM users WHERE id = 2");
  CHECK(cursor != nullptr);
  if (cursor != nullptr) {
    const auto rows = sess_test::collect(*cursor);
    CHECK_EQ(rows.size(), size_t{1});
    if (rows.size() == 1) {
      CHECK_EQ(rows[0][0].as_int(), int64_t{20}); // 原来是 20
    }
  }
}

TEST(TxSession, TransactionControlErrors) {
  auto engine = sess_test::open_engine();
  session::Session session(engine);
  CHECK(sess_test::bootstrap(session, 0));

  session::SessionError error;
  // 没有事务时 COMMIT / ROLLBACK 报错
  CHECK(sess_test::run(session, "COMMIT", &error) == nullptr);
  CHECK(error.code == session::SessionErrorCode::TRANSACTION_ERROR);
  CHECK(sess_test::run(session, "ROLLBACK", &error) == nullptr);
  CHECK(error.code == session::SessionErrorCode::TRANSACTION_ERROR);

  // 嵌套 BEGIN 报错，但不影响外层事务
  CHECK(sess_test::run(session, "BEGIN", &error) != nullptr);
  CHECK(sess_test::run(session, "BEGIN", &error) == nullptr);
  CHECK(error.code == session::SessionErrorCode::TRANSACTION_ERROR);
  CHECK(session.in_transaction()); // 外层还在
  CHECK(sess_test::run(session, "COMMIT", &error) != nullptr);
}

TEST(TxSession, FailInTransactionAbortsItAndRollbackCleansUp) {
  auto engine = sess_test::open_engine();
  session::Session session(engine);
  CHECK(sess_test::bootstrap(session, 2));

  // 制造一条"读到就报错"的坏行（绕过 SQL 层直接写 KV）：
  // 这样下面那条 DELETE 会在**执行期**失败（校验器看不出来）。
  auto table = session.catalog().open_table(sql::Identifier("shop"),
                                            sql::Identifier("users"));
  CHECK(table.has_value());
  if (table.has_value()) {
    const std::string key =
        table->encode_key(sql::Value(int64_t{2}, sql::DataType::INT));
    CHECK(engine->put(key, "garbage") == kv::Status::OK);
  }

  session::SessionError error;
  CHECK(sess_test::run(session, "BEGIN", &error) != nullptr);
  // 先做一次成功的写（进缓冲）
  CHECK(sess_test::run(session,
                       "INSERT INTO users (id, name, age) VALUES (10, 'x', 1)",
                       &error) != nullptr);
  // 再执行一条执行期失败的语句：扫描时读到坏行
  CHECK(sess_test::run(session, "DELETE FROM users", &error) == nullptr);
  CHECK(error.code == session::SessionErrorCode::EXECUTE_ERROR);

  // 事务已经中止：后续语句只允许 ROLLBACK
  CHECK(sess_test::run(session, "SELECT * FROM users", &error) == nullptr);
  CHECK(error.code == session::SessionErrorCode::TRANSACTION_ERROR);
  // 中止状态下 COMMIT 实际执行回滚，并明确报错
  auto commit = sess_test::run(session, "COMMIT", &error);
  CHECK(commit == nullptr);
  CHECK(error.code == session::SessionErrorCode::TRANSACTION_ERROR);
  CHECK(!session.in_transaction());

  // 缓冲里那次成功的 INSERT 也一并回滚：点查确认 id=10 不存在
  // （用点查避开那条坏行，免得又踩到错误）
  auto reopened = session.catalog().open_table(sql::Identifier("shop"),
                                               sql::Identifier("users"));
  CHECK(reopened.has_value());
  if (reopened.has_value()) {
    auto missing = reopened->find(sql::Value(int64_t{10}, sql::DataType::INT));
    CHECK(!missing.has_value());
    if (!missing.has_value()) {
      CHECK(missing.error().code == sql::RelErrorCode::NOT_FOUND);
    }
  }

  // 之后还能正常开新事务
  CHECK(sess_test::run(session, "BEGIN", &error) != nullptr);
  CHECK(sess_test::run(session, "ROLLBACK", &error) != nullptr);
}

TEST(TxSession, StatementAliasesWork) {
  auto engine = sess_test::open_engine();
  session::Session session(engine);
  CHECK(sess_test::bootstrap(session, 0));

  session::SessionError error;
  // START TRANSACTION = BEGIN
  CHECK(sess_test::run(session, "START TRANSACTION", &error) != nullptr);
  CHECK(session.in_transaction());
  // ABORT = ROLLBACK
  CHECK(sess_test::run(session, "ABORT", &error) != nullptr);
  CHECK(!session.in_transaction());
  // END = COMMIT
  CHECK(sess_test::run(session, "BEGIN", &error) != nullptr);
  CHECK(sess_test::run(session, "END", &error) != nullptr);
  CHECK(!session.in_transaction());

  // 带分号、带大小写混写也要认
  CHECK(sess_test::run(session, "begin;", &error) != nullptr);
  CHECK(sess_test::run(session, "Commit ;", &error) != nullptr);
}
