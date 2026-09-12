// tests/test_relation/relation_test_util.h
//
// relation 层测试的公共脚手架：一个打开好的 MockEngine + 常用 schema。
#pragma once

#include <memory>
#include <string>
#include <vector>

#include "relation/kv_catalog.h"
#include "relation/table.h"
#include "sql_types/identifier.h"
#include "sql_types/row.h"
#include "sql_types/schema.h"
#include "sql_types/value.h"
#include "storage/mock_engine/mock_engine.h"

namespace reltest {

// users(id INT PK, name VARCHAR(32) NOT NULL, age INT NULL)
inline sql::TableSchema make_users_schema() {
  sql::TableSchema schema(sql::Identifier("users"));
  schema.add_column(sql::Identifier("id"), sql::DataType::INT, true, false);
  // add_column(name, type, length, pk, nullable)
  schema.add_column(sql::Identifier("name"), sql::DataType::VARCHAR, 32u, false,
                    false);
  schema.add_column(sql::Identifier("age"), sql::DataType::INT, false, true);
  return schema;
}

// accounts(name VARCHAR(16) PK, balance INT)
inline sql::TableSchema make_accounts_schema() {
  sql::TableSchema schema(sql::Identifier("accounts"));
  schema.add_column(sql::Identifier("name"), sql::DataType::VARCHAR, 16u, true,
                    false);
  schema.add_column(sql::Identifier("balance"), sql::DataType::INT, false,
                    true);
  return schema;
}

inline sql::Row users_row(int64_t id, const std::string &name, int64_t age) {
  sql::Row row;
  row.push_back(sql::Value(id, sql::DataType::INT));
  row.push_back(sql::Value(name));
  row.push_back(sql::Value(age, sql::DataType::INT));
  return row;
}

// 打开一个干净的 MockEngine
inline std::shared_ptr<kv::MockEngine> open_engine() {
  auto engine = std::make_shared<kv::MockEngine>();
  kv::DatabaseOptions options;
  options.path = "mock://test";
  engine->open_database(options);
  return engine;
}

} // namespace reltest
