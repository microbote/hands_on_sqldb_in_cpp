// test_executor.cpp
#include <iostream>

#include "condition.h"
#include "executor.h"
#include "mock_engine.h"
#include "plan.h"

using namespace sql;

int main() {
  std::cout << "🧪 测试 Executor" << std::endl;
  std::cout << std::string(50, '=') << std::endl;

  // 1. 创建引擎并初始化数据
  auto engine = storage::StorageEngineFactory::create_engine(
      storage::StorageOptions{storage::StorageType::MOCK});

  // 创建表
  storage::TableSchema schema;
  schema.table_name = "users";
  schema.columns = {{"id", storage::DataType::INTEGER},
                    {"name", storage::DataType::STRING},
                    {"age", storage::DataType::INTEGER},
                    {"email", storage::DataType::STRING}};
  schema.primary_key_index = 0;
  engine->create_table(schema);

  // 插入测试数据
  for (int i = 1; i <= 10; ++i) {
    storage::Row row;
    row.push_back(storage::Value(i));
    row.push_back(storage::Value("user" + std::to_string(i)));
    row.push_back(storage::Value(20 + i));
    row.push_back(storage::Value("user" + std::to_string(i) + "@test.com"));
    engine->insert("users", row);
  }

  Executor executor(engine);

  // 2. 测试索引扫描（点查询）
  {
    std::cout << "\n--- 测试索引扫描（点查询） ---" << std::endl;
    auto plan = std::make_unique<ExecutionPlan>(StatementType::SELECT);
    auto index_plan = std::make_unique<IndexScanPlan>("users");
    index_plan->columns = {"id", "name", "age"};
    index_plan->is_point_query = true;
    index_plan->key_set = {"1", "3", "5"};
    plan->root = std::move(index_plan);

    auto result = executor.execute(plan.get());
    std::cout << "结果: " << (result.success ? "✅" : "❌")
              << " 行数: " << result.rows.size() << std::endl;
  }

  // 3. 测试索引扫描（范围查询）
  {
    std::cout << "\n--- 测试索引扫描（范围查询） ---" << std::endl;
    auto plan = std::make_unique<ExecutionPlan>(StatementType::SELECT);
    auto index_plan = std::make_unique<IndexScanPlan>("users");
    index_plan->columns = {"id", "name", "age"};
    index_plan->is_point_query = false;
    index_plan->key_range = storage::KeyRange("data:users:2", "data:users:7");
    plan->root = std::move(index_plan);

    auto result = executor.execute(plan.get());
    std::cout << "结果: " << (result.success ? "✅" : "❌")
              << " 行数: " << result.rows.size() << std::endl;
  }

  // 4. 测试顺序扫描（带过滤）
  {
    std::cout << "\n--- 测试顺序扫描（带过滤） ---" << std::endl;
    auto plan = std::make_unique<ExecutionPlan>(StatementType::SELECT);
    auto seq_plan = std::make_unique<SequentialScanPlan>("users");
    seq_plan->columns = {"id", "name", "age"};

    // 条件: age > 25
    auto cond = std::make_unique<ConditionExpr>("age", CompareOp::GT,
                                                storage::Value(25));
    seq_plan->filter_condition = std::move(cond);
    plan->root = std::move(seq_plan);

    auto result = executor.execute(plan.get());
    std::cout << "结果: " << (result.success ? "✅" : "❌")
              << " 行数: " << result.rows.size() << std::endl;
  }

  // 5. 测试 INSERT
  {
    std::cout << "\n--- 测试 INSERT ---" << std::endl;
    auto plan = std::make_unique<ExecutionPlan>(StatementType::INSERT);
    auto insert_plan = std::make_unique<InsertPlan>("users");
    insert_plan->columns = {"id", "name", "age", "email"};
    insert_plan->values = {storage::Value(11), storage::Value("Alice"),
                           storage::Value(30),
                           storage::Value("alice@test.com")};
    plan->root = std::move(insert_plan);

    auto result = executor.execute(plan.get());
    std::cout << "结果: " << (result.success ? "✅" : "❌") << std::endl;
  }

  // 6. 测试 UPDATE
  {
    std::cout << "\n--- 测试 UPDATE ---" << std::endl;
    auto plan = std::make_unique<ExecutionPlan>(StatementType::UPDATE);
    auto update_plan = std::make_unique<UpdatePlan>("users");
    update_plan->assignments = {{"age", storage::Value(35)}};
    update_plan->condition =
        std::make_unique<ConditionExpr>("id", CompareOp::EQ, storage::Value(1));
    plan->root = std::move(update_plan);

    auto result = executor.execute(plan.get());
    std::cout << "结果: " << (result.success ? "✅" : "❌") << std::endl;
  }

  // 7. 测试 DELETE
  {
    std::cout << "\n--- 测试 DELETE ---" << std::endl;
    auto plan = std::make_unique<ExecutionPlan>(StatementType::DELETE);
    auto delete_plan = std::make_unique<DeletePlan>("users");
    delete_plan->condition = std::make_unique<ConditionExpr>(
        "id", CompareOp::EQ, storage::Value(11));
    plan->root = std::move(delete_plan);

    auto result = executor.execute(plan.get());
    std::cout << "结果: " << (result.success ? "✅" : "❌") << std::endl;
  }

  // 8. 显示最终数据
  std::cout << "\n--- 最终数据 ---" << std::endl;
  auto rows = engine->scan_all("users");
  for (const auto& row : rows) {
    for (const auto& val : row) {
      std::cout << val.to_string() << " ";
    }
    std::cout << std::endl;
  }

  return 0;
}