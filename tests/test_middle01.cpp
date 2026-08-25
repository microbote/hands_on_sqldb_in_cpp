// main.cpp
#include <iostream>
#include <memory>

#include "ast.h"
#include "executor.h"
#include "leveldb_engine.h"
#include "optimizer.h"
#include "statement_builder.h"

// Flex/Bison 外部声明
extern int yyparse();
extern void yy_scan_string(const char* str);
extern void yy_delete_buffer(YY_BUFFER_STATE buffer);
extern YY_BUFFER_STATE yy_scan_string(const char* str);
extern ASTNode* g_parsed_ast;

void init_test_data(std::shared_ptr<storage::StorageEngine> engine) {
  // 创建 users 表
  if (!engine->table_exists("users")) {
    storage::TableSchema schema;
    schema.table_name = "users";
    schema.columns = {{"id", storage::DataType::INTEGER},
                      {"name", storage::DataType::STRING},
                      {"age", storage::DataType::INTEGER},
                      {"email", storage::DataType::STRING}};
    engine->create_table(schema);

    // 插入测试数据
    for (int i = 1; i <= 10; ++i) {
      storage::Row row;
      row.push_back(storage::Value(i));
      row.push_back(storage::Value("user" + std::to_string(i)));
      row.push_back(storage::Value(18 + i));
      row.push_back(storage::Value("user" + std::to_string(i) + "@test.com"));
      engine->insert("users", row);
    }
    std::cout << "✅ 创建表 users，插入 10 行数据" << std::endl;
  }
}

void execute_sql(const std::string& sql,
                 std::shared_ptr<storage::StorageEngine> engine) {
  std::cout << "\n" << std::string(60, '=') << std::endl;
  std::cout << "SQL: " << sql << std::endl;
  std::cout << std::string(60, '=') << std::endl;

  // 1. 词法/语法分析
  YY_BUFFER_STATE buffer = yy_scan_string(sql.c_str());
  int parse_result = yyparse();
  yy_delete_buffer(buffer);

  if (parse_result != 0 || !g_parsed_ast) {
    std::cout << "❌ 语法解析失败" << std::endl;
    return;
  }
  std::cout << "✅ 语法解析成功" << std::endl;

  // 2. 构建 + 验证
  sql::StatementBuilder builder(engine);
  auto stmt = builder.build(g_parsed_ast);
  if (!stmt) {
    std::cout << "❌ 构建/验证失败: " << builder.get_error() << std::endl;
    free_ast(g_parsed_ast);
    g_parsed_ast = NULL;
    return;
  }
  std::cout << "✅ Statement: " << stmt->to_string() << std::endl;

  // 3. 优化
  sql::Optimizer optimizer(engine);
  auto plan = optimizer.optimize(stmt.get());
  if (!plan) {
    std::cout << "❌ 优化失败" << std::endl;
    free_ast(g_parsed_ast);
    g_parsed_ast = NULL;
    return;
  }
  std::cout << "✅ Plan: " << plan->to_string() << std::endl;

  // 4. 执行
  sql::Executor executor(engine);
  auto result = executor.execute(plan.get());

  if (result.success) {
    std::cout << "\n✅ 执行成功: " << result.message << std::endl;
    if (result.affected_rows > 0) {
      std::cout << "   受影响行数: " << result.affected_rows << std::endl;
    }
  } else {
    std::cout << "\n❌ 执行失败: " << result.message << std::endl;
  }

  free_ast(g_parsed_ast);
  g_parsed_ast = NULL;
}

int main() {
  std::cout << "╔══════════════════════════════════════════╗" << std::endl;
  std::cout << "║     SQL 引擎 (中端完整版)               ║" << std::endl;
  std::cout << "╚══════════════════════════════════════════╝" << std::endl;

  // 创建存储引擎
  auto engine =
      storage::StorageEngineFactory::create_leveldb_engine("./sql_db");
  if (!engine) {
    std::cerr << "❌ 无法创建存储引擎" << std::endl;
    return 1;
  }
  std::cout << "✅ 存储引擎已启动" << std::endl;

  // 初始化数据
  init_test_data(engine);

  // 测试用例
  std::vector<std::string> tests = {
      "SELECT * FROM users;",
      "SELECT id, name FROM users WHERE age > 22;",
      "SELECT id, name, age FROM users WHERE age > 20 AND age < 25;",
      "INSERT INTO users (id, name, age, email) VALUES (11, 'Alice', 30, "
      "'alice@test.com');",
      "SELECT * FROM users WHERE name = 'Alice';",
      "UPDATE users SET age = 35 WHERE id = 1;",
      "SELECT * FROM users WHERE id = 1;",
      "DELETE FROM users WHERE age > 30;",
      "SELECT * FROM users;"};

  for (const auto& sql : tests) {
    execute_sql(sql, engine);
  }

  engine->close();
  std::cout << "\n👋 完成" << std::endl;
  return 0;
}