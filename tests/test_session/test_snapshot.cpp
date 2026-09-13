// tests/test_session/test_snapshot.cpp
//
// 只读事务的**可重复读**：`BEGIN` 取快照（leveldb::Snapshot / Mock
// 的一份拷贝）， 事务里的读永远是 begin 那一刻的版本；而且只读事务**不占写槽**
// —— 读者不阻塞写者（这就是选 Snapshot 而不是"读锁持有到结束"的理由）。
//
// 这里用两个 session（两条连接共享一份存储）做真正的并发交错：
// A 开着只读事务，B 照常写并提交。
#include "test_framework.h"

#include <string>

#include "session_test_util.h"

namespace {

std::vector<int64_t> ints(session::Session &session, const std::string &sql,
                          bool *ok = nullptr) {
  return sess_test::first_column_ints(session, sql, ok);
}

} // namespace

TEST(SnapshotRead, RepeatableReadAcrossStatements) {
  auto engines = sess_test::open_two_engines();
  session::Session a(engines.first);
  session::Session b(engines.second);
  CHECK(sess_test::bootstrap(a, 1)); // 两个 session 看同一份存储
  CHECK(sess_test::use_database(b)); // USE 是会话状态，b 也要指一次

  bool ok = false;
  auto ages = ints(a, "SELECT age FROM users WHERE id = 1", &ok);
  CHECK(ok);
  CHECK_EQ(ages.size(), size_t{1});
  const int64_t before = ages.empty() ? 0 : ages[0];

  // A 开只读事务：拿快照
  session::SessionError error;
  CHECK(sess_test::run(a, "BEGIN", &error) != nullptr);
  auto first_read = ints(a, "SELECT age FROM users WHERE id = 1", &ok);
  CHECK(ok);

  // B 改并提交（A 只读，不挡 B）
  CHECK(sess_test::run(b, "UPDATE users SET age = 999 WHERE id = 1", &error) !=
        nullptr);
  auto b_read = ints(b, "SELECT age FROM users WHERE id = 1", &ok);
  CHECK(ok);
  CHECK_EQ(b_read.size(), size_t{1});
  if (b_read.size() == 1) {
    CHECK_EQ(b_read[0], int64_t{999});
  }

  // A 再读：还是 begin 那一刻的版本（可重复读）
  auto second_read = ints(a, "SELECT age FROM users WHERE id = 1", &ok);
  CHECK(ok);
  CHECK_EQ(second_read, first_read);
  if (second_read.size() == 1) {
    CHECK_EQ(second_read[0], before);
  }
  // 扫描也一样：A 的这条事务看不到 B 的改动
  const auto all = ints(a, "SELECT age FROM users", &ok);
  CHECK(ok);
  CHECK_EQ(all, first_read);

  // 事务结束 -> 看最新
  CHECK(sess_test::run(a, "COMMIT", &error) != nullptr);
  auto after = ints(a, "SELECT age FROM users WHERE id = 1", &ok);
  CHECK(ok);
  CHECK_EQ(after.size(), size_t{1});
  if (after.size() == 1) {
    CHECK_EQ(after[0], int64_t{999});
  }
}

TEST(SnapshotRead, ReadOnlyTransactionDoesNotBlockWriters) {
  auto engines = sess_test::open_two_engines();
  session::Session a(engines.first);
  session::Session b(engines.second);
  CHECK(sess_test::bootstrap(a, 1));
  CHECK(sess_test::use_database(b));

  session::SessionError error;
  CHECK(sess_test::run(a, "BEGIN", &error) != nullptr);
  bool ok = false;
  CHECK_EQ(ints(a, "SELECT id FROM users", &ok).size(), size_t{1});
  CHECK(ok);

  // B 的写语句照样成功（A 只读、不占写槽）
  CHECK(sess_test::run(b,
                       "INSERT INTO users (id, name, age) VALUES (2, 'b', 20)",
                       &error) != nullptr);
  CHECK(sess_test::run(b, "BEGIN", &error) != nullptr);
  CHECK(sess_test::run(b, "ROLLBACK", &error) != nullptr);

  // A 的快照里仍然只有一行；提交后看到两行
  CHECK_EQ(ints(a, "SELECT id FROM users", &ok).size(), size_t{1});
  CHECK(ok);
  CHECK(sess_test::run(a, "COMMIT", &error) != nullptr);
  CHECK_EQ(ints(a, "SELECT id FROM users", &ok).size(), size_t{2});
  CHECK(ok);
}

TEST(SnapshotRead, SecondWriterIsStillRejected) {
  auto engines = sess_test::open_two_engines();
  session::Session a(engines.first);
  session::Session b(engines.second);
  CHECK(sess_test::bootstrap(a, 1));
  CHECK(sess_test::use_database(b));

  session::SessionError error;
  CHECK(sess_test::run(a, "BEGIN", &error) != nullptr);
  // A 一写就拿到写槽（单写者）
  CHECK(sess_test::run(a,
                       "INSERT INTO users (id, name, age) VALUES (2, 'a', 20)",
                       &error) != nullptr);

  // B 的写被拒（读还是可以的）
  auto write = sess_test::run(
      b, "INSERT INTO users (id, name, age) VALUES (3, 'b', 30)", &error);
  CHECK(write == nullptr);
  CHECK(error.code == session::SessionErrorCode::TRANSACTION_ERROR);
  CHECK(error.message.find("busy") != std::string::npos);
  bool ok = false;
  CHECK_EQ(ints(b, "SELECT id FROM users", &ok).size(), size_t{1});
  CHECK(ok);

  // A 结束 -> B 可以写了
  CHECK(sess_test::run(a, "ROLLBACK", &error) != nullptr);
  CHECK(sess_test::run(b,
                       "INSERT INTO users (id, name, age) VALUES (3, 'b', 30)",
                       &error) != nullptr);
}

TEST(SnapshotRead, SnapshotAlsoCoversTheCatalog) {
  // 元数据（schema / 表名单）读的也是这条连接 -> 同一个快照：
  // 事务里看不到别人新建的表，事务结束后看得到。
  auto engines = sess_test::open_two_engines();
  session::Session a(engines.first);
  session::Session b(engines.second);
  CHECK(sess_test::bootstrap(a, 0));
  CHECK(sess_test::use_database(b));

  session::SessionError error;
  CHECK(sess_test::run(a, "BEGIN", &error) != nullptr);

  CHECK(sess_test::run(b, "CREATE TABLE logs (id INT PRIMARY KEY)", &error) !=
        nullptr);

  // A 的快照里没有 logs 这张表
  auto before = sess_test::run(a, "SELECT id FROM logs", &error);
  CHECK(before == nullptr);
  CHECK(error.code == session::SessionErrorCode::VALIDATE_ERROR);

  // B 自己看得到
  CHECK(sess_test::run(b, "SELECT id FROM logs", &error) != nullptr);

  // A 提交后（新语句）也看得到
  CHECK(sess_test::run(a, "COMMIT", &error) != nullptr);
  CHECK(sess_test::run(a, "SELECT id FROM logs", &error) != nullptr);
}

TEST(SnapshotRead, AutocommitStatementsAlwaysSeeTheLatest) {
  auto engines = sess_test::open_two_engines();
  session::Session a(engines.first);
  session::Session b(engines.second);
  CHECK(sess_test::bootstrap(a, 1));
  CHECK(sess_test::use_database(b));

  session::SessionError error;
  CHECK(sess_test::run(b, "UPDATE users SET age = 7 WHERE id = 1", &error) !=
        nullptr);

  // 没有事务：每条语句看最新已提交状态
  bool ok = false;
  const auto ages = ints(a, "SELECT age FROM users WHERE id = 1", &ok);
  CHECK(ok);
  CHECK_EQ(ages.size(), size_t{1});
  if (ages.size() == 1) {
    CHECK_EQ(ages[0], int64_t{7});
  }
}
