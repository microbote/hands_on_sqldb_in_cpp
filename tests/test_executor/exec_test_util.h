// tests/test_executor/exec_test_util.h
//
// 端到端脚手架：SQL -> Parser -> StatementBuilder -> Optimizer -> Planner
//              -> exec::execute -> 客户端游标。
#pragma once

#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "executor/executor.h"
#include "parser/parser.h"
#include "planner/optimizer.h"
#include "planner/planner.h"
#include "relation/kv_catalog.h"
#include "sql_types/identifier.h"
#include "sql_types/row.h"
#include "sql_types/schema.h"
#include "sql_types/value.h"
#include "statement/stmt_builder.h"
#include "storage/mock_engine/mock_engine.h"

namespace exectest {

// users(id INT PK, name VARCHAR(32) NOT NULL, age INT NULL)
inline sql::TableSchema users_schema() {
  sql::TableSchema schema(sql::Identifier("users"));
  schema.add_column(sql::Identifier("id"), sql::DataType::INT, true, false);
  schema.add_column(sql::Identifier("name"), sql::DataType::VARCHAR, 32u, false,
                    false);
  schema.add_column(sql::Identifier("age"), sql::DataType::INT, false, true);
  return schema;
}

inline std::shared_ptr<kv::MockEngine> open_engine() {
  auto engine = std::make_shared<kv::MockEngine>();
  kv::DatabaseOptions options;
  options.path = "mock://executor-test";
  engine->open_database(options);
  return engine;
}

inline sql::Row users_row(int64_t id, const std::string &name, int64_t age) {
  sql::Row row;
  row.push_back(sql::Value(id, sql::DataType::INT));
  row.push_back(sql::Value(name));
  row.push_back(sql::Value(age, sql::DataType::INT));
  return row;
}

// 建库建表
inline bool setup(sql::KVCatalog &catalog, const char *db = "testdb") {
  if (!catalog.create_database(sql::Identifier(db))) {
    return false;
  }
  if (!catalog.create_table(sql::Identifier(db), users_schema())) {
    return false;
  }
  return catalog.use_database(sql::Identifier(db));
}

// id = 1..count，name = "userN"，age = id * 10
inline bool seed_users(sql::KVCatalog &catalog, int count,
                       const char *db = "testdb") {
  auto table =
      catalog.open_table(sql::Identifier(db), sql::Identifier("users"));
  if (!table.has_value()) {
    return false;
  }
  for (int id = 1; id <= count; ++id) {
    if (!table
             ->insert(users_row(id, "user" + std::to_string(id),
                                static_cast<int64_t>(id) * 10))
             .has_value()) {
      return false;
    }
  }
  return true;
}

// SQL -> Query
inline std::optional<sql::Query> build_query(const std::string &sql) {
  parser::Parser parser;
  const std::string terminated = sql.back() == ';' ? sql : sql + ";";
  auto parsed = parser.parse(terminated);
  if (!parsed.success || parsed.ast == nullptr) {
    return std::nullopt;
  }
  stmt::StatementBuilder builder;
  auto built = builder.build(parsed.ast.get());
  if (!built.has_value()) {
    return std::nullopt;
  }
  return std::optional<sql::Query>(std::move(*built));
}

// 计划里目标表的库名/表名（扫描节点或写节点上都带着）
inline sql::Identifier plan_db(const plan::PlanNode &root) {
  for (const plan::PlanNode *node = &root; node != nullptr;
       node = node->child()) {
    if (const auto *query_node =
            dynamic_cast<const plan::QueryPlanNode *>(node)) {
      if (!query_node->query().db.empty()) {
        return query_node->query().db;
      }
    }
    if (const auto *scan = dynamic_cast<const plan::ScanPlan *>(node)) {
      if (!scan->target().db.empty()) {
        return scan->target().db;
      }
    }
  }
  return sql::Identifier();
}

inline sql::Identifier plan_table(const plan::PlanNode &root) {
  for (const plan::PlanNode *node = &root; node != nullptr;
       node = node->child()) {
    if (const auto *query_node =
            dynamic_cast<const plan::QueryPlanNode *>(node)) {
      if (!query_node->query().table.empty()) {
        return query_node->query().table;
      }
    }
    if (const auto *scan = dynamic_cast<const plan::ScanPlan *>(node)) {
      if (!scan->target().table.empty()) {
        return scan->target().table;
      }
    }
  }
  return sql::Identifier();
}

// 跑一条 DML 语句；失败时返回 nullptr 并把原因写进 error
inline std::unique_ptr<exec::ResultCursor>
run_sql(sql::KVCatalog &catalog, const std::string &sql,
        std::string *error = nullptr) {
  const auto fail = [&](const std::string &message) {
    if (error != nullptr) {
      *error = message;
    }
    return std::unique_ptr<exec::ResultCursor>(nullptr);
  };

  auto query = build_query(sql);
  if (!query.has_value()) {
    return fail("parse/build failed: " + sql);
  }
  plan::Optimizer optimizer(catalog);
  auto optimized = optimizer.optimize(*query);
  if (!optimized.has_value()) {
    return fail("optimize failed: " + optimized.error().to_string());
  }
  plan::Planner planner;
  auto planned = planner.plan(std::move(*optimized));
  if (!planned.has_value()) {
    return fail("plan failed: " + planned.error().to_string());
  }

  auto table = catalog.open_table(plan_db(**planned), plan_table(**planned));
  if (!table.has_value()) {
    return fail("open table failed: " + table.error().to_string());
  }
  auto shared_table = std::make_shared<sql::Table>(std::move(*table));
  auto cursor = exec::execute(**planned, shared_table);
  if (!cursor.has_value()) {
    return fail("execute failed: " + cursor.error().to_string());
  }
  return std::move(*cursor);
}

// 收完游标里的所有行；出错时把错误写进 error
inline std::vector<sql::Row> collect_rows(exec::ResultCursor &cursor,
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

// 取第 column 列的整数值（测试里最常用）
inline std::vector<int64_t> column_ints(exec::ResultCursor &cursor,
                                        size_t column = 0,
                                        sql::CursorError *error = nullptr) {
  std::vector<int64_t> values;
  for (const auto &row : collect_rows(cursor, error)) {
    if (column < row.size() && !row[column].is_null()) {
      values.push_back(row[column].as_int());
    }
  }
  return values;
}

} // namespace exectest
