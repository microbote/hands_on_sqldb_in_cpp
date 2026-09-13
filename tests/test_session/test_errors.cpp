// tests/test_session/test_errors.cpp
//
// 错误路径：错误码分档、出错位置与高亮片段。
#include "test_framework.h"

#include <string>

#include "session_test_util.h"

namespace {

session::SessionError expect_error(class session::Session &session,
                                   const std::string &sql) {
  session::SessionError error;
  auto cursor = sess_test::run(session, sql, &error);
  CHECK(cursor == nullptr);
  return error;
}

} // namespace

TEST(SessionError, EmptySqlIsRejected) {
  auto engine = sess_test::open_engine();
  session::Session session(engine);

  for (const std::string &sql :
       {std::string(), std::string("   "), std::string(";")}) {
    const auto error = expect_error(session, sql);
    CHECK(error.code == session::SessionErrorCode::EMPTY_SQL);
  }
}

TEST(SessionError, SyntaxErrorCarriesPositionAndHighlight) {
  auto engine = sess_test::open_engine();
  session::Session session(engine);

  const auto error = expect_error(session, "SELCT 1");
  CHECK(error.code == session::SessionErrorCode::PARSE_ERROR);
  CHECK(!error.message.empty());
  CHECK(sspan_valid(error.span)); // 有位置
  CHECK(error.to_string().find("line") != std::string::npos);
  // 高亮片段来自 stmt::highlight_span（colors=false 时只标出片段）
  CHECK(!error.highlight(false).empty());
}

TEST(SessionError, TableNotFoundIsValidationErrorWithSpan) {
  auto engine = sess_test::open_engine();
  session::Session session(engine);
  CHECK(sess_test::bootstrap(session, 1));

  const auto error = expect_error(session, "SELECT * FROM missing");
  CHECK(error.code == session::SessionErrorCode::VALIDATE_ERROR);
  CHECK(sspan_valid(error.span)); // 定位到表名
  CHECK(error.highlight(false).find("missing") != std::string::npos);
}

TEST(SessionError, UnknownColumnIsValidationError) {
  auto engine = sess_test::open_engine();
  session::Session session(engine);
  CHECK(sess_test::bootstrap(session, 1));

  const auto error = expect_error(session, "SELECT nope FROM users");
  CHECK(error.code == session::SessionErrorCode::VALIDATE_ERROR);
}

TEST(SessionError, NoDatabaseSelectedIsReported) {
  auto engine = sess_test::open_engine();
  session::Session session(engine);
  // 没有任何 USE，直接查表
  CHECK(sess_test::run(session, "CREATE DATABASE shop") != nullptr);
  const auto error = expect_error(session, "SELECT * FROM users");
  CHECK(!error.ok());
  CHECK(error.code == session::SessionErrorCode::VALIDATE_ERROR ||
        error.code == session::SessionErrorCode::OPTIMIZE_ERROR);
  CHECK(!error.message.empty());
}

TEST(SessionError, UseUnknownDatabaseFails) {
  auto engine = sess_test::open_engine();
  session::Session session(engine);
  const auto error = expect_error(session, "USE nope");
  CHECK(error.code == session::SessionErrorCode::VALIDATE_ERROR);
}

TEST(SessionError, DuplicateCreateTableFails) {
  auto engine = sess_test::open_engine();
  session::Session session(engine);
  CHECK(sess_test::bootstrap(session, 0));

  const auto error =
      expect_error(session, "CREATE TABLE users (id INT PRIMARY KEY, name "
                            "VARCHAR(32) NOT NULL, age INT)");
  CHECK(error.code == session::SessionErrorCode::VALIDATE_ERROR);
}

TEST(SessionError, InsertMissingNotNullColumnFails) {
  auto engine = sess_test::open_engine();
  session::Session session(engine);
  CHECK(sess_test::bootstrap(session, 0));

  // name 是 NOT NULL：只给 id 必须失败
  session::SessionError error;
  auto cursor =
      sess_test::run(session, "INSERT INTO users (id) VALUES (1)", &error);
  CHECK(cursor == nullptr);
  CHECK(!error.ok());
}

TEST(SessionError, MultipleStatementsInOneCallAreRejected) {
  auto engine = sess_test::open_engine();
  session::Session session(engine);
  CHECK(sess_test::bootstrap(session, 1));

  // execute() 只跑一条语句：多余的内容必须报错，而不是被悄悄忽略
  const auto error =
      expect_error(session, "SELECT * FROM users; SELECT * FROM users;");
  CHECK(error.code == session::SessionErrorCode::PARSE_ERROR);
}

TEST(SessionError, UnsupportedStatementIsRejected) {
  auto engine = sess_test::open_engine();
  session::Session session(engine);
  // 语法能过、但这一层不处理的语句（这里用未知语句类型模拟）
  const auto error = expect_error(session, "SELECT");
  CHECK(!error.ok());
}

TEST(SessionError, ClosedEngineIsReported) {
  auto engine = std::make_shared<kv::MockEngine>(); // 没 open
  session::Session session(engine);
  const auto error = expect_error(session, "SELECT 1");
  CHECK(error.code == session::SessionErrorCode::EXECUTE_ERROR);
}
