// tests/test_session/session_test_util.h
#pragma once

#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "executor/executor.h"
#include "session/session.h"
#include "sql_types/row.h"
#include "storage/mock_engine/mock_engine.h"

namespace sess_test {

inline std::shared_ptr<kv::MockEngine> open_engine() {
  auto engine = std::make_shared<kv::MockEngine>();
  kv::DatabaseOptions options;
  options.path = "mock://session-test";
  engine->open_database(options);
  return engine;
}

// 跑一条 SQL；失败时把错误信息写进 error（测试里断言用）
inline std::unique_ptr<exec::ResultCursor>
run(session::Session &session, const std::string &sql,
    session::SessionError *error = nullptr) {
  auto result = session.execute(sql);
  if (!result.has_value()) {
    if (error != nullptr) {
      *error = result.error();
    }
    return nullptr;
  }
  return std::move(*result);
}

inline std::vector<sql::Row> collect(exec::ResultCursor &cursor,
                                     sql::CursorError *error = nullptr) {
  std::vector<sql::Row> rows;
  while (true) {
    auto row = cursor.next();
    if (row.has_value()) {
      rows.push_back(std::move(*row));
      continue;
    }
    if (error != nullptr) {
      *error = row.error();
    }
    break;
  }
  return rows;
}

// SELECT 的便利封装：收集所有行的第一列整数
inline std::vector<int64_t> first_column_ints(session::Session &session,
                                              const std::string &sql,
                                              bool *ok = nullptr) {
  session::SessionError error;
  auto cursor = run(session, sql, &error);
  if (cursor == nullptr) {
    if (ok != nullptr) {
      *ok = false;
    }
    return {};
  }
  sql::CursorError cursor_error;
  std::vector<int64_t> values;
  for (const auto &row : collect(*cursor, &cursor_error)) {
    if (row.size() > 0 && !row[0].is_null()) {
      values.push_back(row[0].as_int());
    }
  }
  if (ok != nullptr) {
    *ok = cursor_error.end();
  }
  return values;
}

// 建库 + 建表 + 塞数据（id = 1..count, name = "userN", age = id*10）
inline bool bootstrap(session::Session &session, int count = 5) {
  session::SessionError error;
  if (run(session, "CREATE DATABASE shop", &error) == nullptr) {
    return false;
  }
  if (run(session, "USE shop", &error) == nullptr) {
    return false;
  }
  if (run(session,
          "CREATE TABLE users (id INT PRIMARY KEY, name VARCHAR(32) NOT NULL, "
          "age INT)",
          &error) == nullptr) {
    return false;
  }
  for (int id = 1; id <= count; ++id) {
    const std::string sql =
        "INSERT INTO users (id, name, age) VALUES (" + std::to_string(id) +
        ", 'user" + std::to_string(id) + "', " + std::to_string(id * 10) + ")";
    if (run(session, sql, &error) == nullptr) {
      return false;
    }
  }
  return true;
}

} // namespace sess_test
