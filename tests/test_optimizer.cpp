// test_optimizer.cpp
#include <iostream>
#include <memory>

#include "mock_engine.h"
#include "optimizer.h"
#include "statement.h"

using namespace sql;

int main() {
  std::cout << "🧪 测试 Optimizer" << std::endl;
  std::cout << std::string(50, '=') << std::endl;

  auto engine = storage::StorageEngineFactory::create_mock_engine();

  // 创建测试表
  storage::TableSchema schema;
  schema.table_name = "users";
  schema.columns = {{"id", storage::DataType::INTEGER},
                    {"name", storage::DataType::STRING},
                    {"age", storage::DataType::INTEGER}};
  engine->create_table(schema);

  // 插入测试数据
  for (int i = 1; i <= 10; ++i) {
    storage::Row row;
    row.push_back(storage::Value(i));
    row.push_back(storage::Value("user" + std::to_string(i)));
    row.push_back(storage::Value(20 + i));
    engine->insert("users", row);
  }

  Optimizer optimizer(engine);

  // 测试 SELECT 语句
  auto select_stmt = std::make_unique<SelectStatement>("users");
  select_stmt->columns = {"id", "name", "age"};
  select_stmt->conditions = {{"age", CompareOp::GT, Value(22)},
                             {"age", CompareOp::LT, Value(28)}};

  std::cout << "\n优化 SELECT 语句:" << std::endl;
  std::cout << "  " << select_stmt->to_string() << std::endl;

  auto plan = optimizer.optimize(select_stmt.get());
  if (plan) {
    std::cout << "  ✅ 计划: " << plan->to_string() << std::endl;
    std::cout << "  代价: " << plan->estimated_cost.total() << std::endl;
  } else {
    std::cout << "  ❌ 优化失败" << std::endl;
  }

  // 测试 INSERT
  auto insert_stmt = std::make_unique<InsertStatement>("users");
  insert_stmt->columns = {"id", "name", "age"};
  insert_stmt->values = {Value(11), Value("Bob"), Value(25)};

  std::cout << "\n优化 INSERT 语句:" << std::endl;
  std::cout << "  " << insert_stmt->to_string() << std::endl;

  plan = optimizer.optimize(insert_stmt.get());
  if (plan) {
    std::cout << "  ✅ 计划: " << plan->to_string() << std::endl;
  } else {
    std::cout << "  ❌ 优化失败" << std::endl;
  }

  return 0;
}