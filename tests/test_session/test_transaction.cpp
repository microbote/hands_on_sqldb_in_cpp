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

TEST(TxSession, WorkKeywordFormsAreAccepted) {
  // BEGIN WORK / COMMIT WORK / ROLLBACK WORK 是标准写法：
  // 语法层认出来（老的文本层匹配只认六个整串，这些形式会被当成语法错误）
  auto engine = sess_test::open_engine();
  session::Session session(engine);
  CHECK(sess_test::bootstrap(session, 0));

  session::SessionError error;
  CHECK(sess_test::run(session, "BEGIN WORK", &error) != nullptr);
  CHECK(session.in_transaction());
  CHECK(sess_test::run(session, "ROLLBACK WORK", &error) != nullptr);
  CHECK(!session.in_transaction());

  CHECK(sess_test::run(session, "BEGIN WORK;", &error) != nullptr);
  CHECK(sess_test::run(session, "COMMIT WORK;", &error) != nullptr);
  CHECK(!session.in_transaction());
}

TEST(TxSession, GrammarOwnsTransactionDiagnostics) {
  auto engine = sess_test::open_engine();
  session::Session session(engine);
  CHECK(sess_test::bootstrap(session, 1));

  // 不支持的形式由语法层给出**具体原因**（而不是笼统的 syntax error）
  session::SessionError error;
  CHECK(sess_test::run(session, "COMMIT AND CHAIN", &error) == nullptr);
  CHECK(error.code == session::SessionErrorCode::PARSE_ERROR);
  CHECK(error.message.find("AND CHAIN") != std::string::npos);

  CHECK(sess_test::run(session, "ROLLBACK TO SAVEPOINT sp1", &error) ==
        nullptr);
  CHECK(error.code == session::SessionErrorCode::PARSE_ERROR);
  CHECK(error.message.find("SAVEPOINT") != std::string::npos);

  CHECK(sess_test::run(session, "BEGIN DEFERRED", &error) == nullptr);
  CHECK(error.code == session::SessionErrorCode::PARSE_ERROR);
  CHECK(error.message.find("transaction modes") != std::string::npos);

  // 事务关键字现在被保留（和 END/DESC 一样）：不能当表名/列名用
  CHECK(sess_test::run(session, "SELECT commit FROM users", &error) == nullptr);
  CHECK(error.code == session::SessionErrorCode::PARSE_ERROR);
  // 但只认整词：BEGINNER 只是普通标识符（这里因为不是语句开头而语法错）
  CHECK(sess_test::run(session, "BEGINNER", &error) == nullptr);
  CHECK(error.code == session::SessionErrorCode::PARSE_ERROR);
  CHECK(error.message.find("transaction modes") == std::string::npos);
}

TEST(TxSession, AbortedTransactionMasksSyntaxErrorsToo) {
  auto engine = sess_test::open_engine();
  session::Session session(engine);
  CHECK(sess_test::bootstrap(session, 2));

  // 造一条坏行，让 DELETE 在执行期失败（校验器看不出来）
  auto table = session.catalog().open_table(sql::Identifier("shop"),
                                            sql::Identifier("users"));
  CHECK(table.has_value());
  if (table.has_value()) {
    const std::string key =
        table->encode_key(sql::Value(int64_t{2}, sql::DataType::INT));
    CHECK(engine->put(key, "garbage") == kv::Status::OK);
  }

  session::SessionError error;
  CHECK(sess_test::run(session, "BEGIN WORK", &error) != nullptr);
  CHECK(sess_test::run(session, "DELETE FROM users", &error) == nullptr);
  CHECK(error.code == session::SessionErrorCode::EXECUTE_ERROR);

  // 事务已中止：连语法错误也被"事务已中止"盖住（Postgres 风格）——
  // 否则用户会以为"改一下语法就能继续"，实际上必须 ROLLBACK
  CHECK(sess_test::run(session, "SELCT 1", &error) == nullptr);
  CHECK(error.code == session::SessionErrorCode::TRANSACTION_ERROR);
  CHECK(error.message.find("aborted") != std::string::npos);

  // 事务控制语句例外：中止状态下 ROLLBACK 仍然必须能跑
  CHECK(sess_test::run(session, "ROLLBACK WORK", &error) != nullptr);
  CHECK(!session.in_transaction());
}

TEST(TxSession, ExplainIsRejectedInAnAbortedTransaction) {
  auto engine = sess_test::open_engine();
  session::Session session(engine);
  CHECK(sess_test::bootstrap(session, 2));

  // 造一条坏行让 DELETE 在执行期失败 -> 事务中止
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
  CHECK(sess_test::run(session, "DELETE FROM users", &error) == nullptr);
  CHECK(error.code == session::SessionErrorCode::EXECUTE_ERROR);

  // 中止的事务里连 EXPLAIN 也不放行（ANALYZE 会真的读数据）
  auto explained =
      sess_test::explain(session, "EXPLAIN ANALYZE SELECT * FROM users");
  CHECK(!explained.ok);
  CHECK(explained.error.code == session::SessionErrorCode::TRANSACTION_ERROR);

  CHECK(sess_test::run(session, "ROLLBACK", &error) != nullptr);
}

TEST(TxSession, ExplainOnTransactionStatementIsRejected) {
  auto engine = sess_test::open_engine();
  session::Session session(engine);
  CHECK(sess_test::bootstrap(session, 0));

  // 事务控制语句没有计划树：EXPLAIN 不能编造一个（语法层认得 BEGIN，
  // 所以这里的提示比"语法错误"具体）
  auto explained = sess_test::explain(session, "EXPLAIN BEGIN;");
  CHECK(!explained.ok);
  CHECK(explained.error.code == session::SessionErrorCode::NOT_SUPPORTED);
  CHECK(explained.error.message.find("BEGIN") != std::string::npos);
  CHECK(!session.in_transaction()); // EXPLAIN 没有真的开事务
}
