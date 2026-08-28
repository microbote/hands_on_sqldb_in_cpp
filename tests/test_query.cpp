// main.cpp (完整流程示例)
#include <iostream>
#include <memory>

#include "parser/ast.h"
#include "query/executor.h"
#include "query/optimizer.h"
#include "query/statement_builder.h"
#include "sql/database_manager.h"
#include "storage/kvengine/mock_engine.h"

// Flex/Bison 外部声明
extern int yyparse();
extern void yy_scan_string(const char* str);
extern void yy_delete_buffer(YY_BUFFER_STATE buffer);
extern YY_BUFFER_STATE yy_scan_string(const char* str);
extern ASTNode* g_parsed_ast;

void execute_sql(const std::string& sql,
                 std::shared_ptr<sql::DatabaseManager> db_manager,
                 const std::string& current_db = "") {
  std::cout << "\n" << std::string(60, '=') << std::endl;
  std::cout << "SQL: " << sql << std::endl;
  std::cout << std::string(60, '=') << std::endl;

  // ============================================================
  // 1. 词法/语法分析 (Parser Layer)
  // ============================================================
  YY_BUFFER_STATE buffer = yy_scan_string(sql.c_str());
  int parse_result = yyparse();
  yy_delete_buffer(buffer);

  if (parse_result != 0 || !g_parsed_ast) {
    std::cout << "❌ 语法解析失败" << std::endl;
    return;
  }
  std::cout << "✅ 语法解析成功" << std::endl;

  // 打印 AST (调试)
  std::cout << "\n📊 AST:" << std::endl;
  print_ast(g_parsed_ast, 2);

  // ============================================================
  // 2. 构建 Statement (Query Processing Layer - Step 1)
  // ============================================================
  query::StatementBuilder builder(db_manager, current_db);
  auto stmt = builder.build(g_parsed_ast);

  if (!stmt) {
    std::cout << "❌ 构建 Statement 失败: " << builder.get_error() << std::endl;
    free_ast(g_parsed_ast);
    g_parsed_ast = NULL;
    return;
  }

  std::cout << "\n📝 Statement: " << stmt->to_string() << std::endl;

  // ============================================================
  // 3. 优化 (Query Processing Layer - Step 2)
  // ============================================================
  query::Optimizer optimizer(db_manager);
  auto plan = optimizer.optimize(stmt.get(), current_db);

  if (!plan) {
    std::cout << "❌ 优化失败" << std::endl;
    free_ast(g_parsed_ast);
    g_parsed_ast = NULL;
    return;
  }

  std::cout << "\n📋 Plan: " << plan->to_string() << std::endl;

  // ============================================================
  // 4. 执行 (Query Processing Layer - Step 3)
  // ============================================================
  query::Executor executor(db_manager);
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
  std::cout << "║     SQL 引擎 - 完整流程                 ║" << std::endl;
  std::cout << "╚══════════════════════════════════════════╝" << std::endl;

  // 1. 初始化存储引擎
  auto engine = std::make_shared<kv::MockEngine>();
  engine->open_database(kv::DatabaseOptions().set_path("./sql_db"));

  // 2. 初始化 SQL 关系层
  auto db_manager = std::make_shared<sql::DatabaseManager>(engine);

  // 3. 创建测试数据库和表
  db_manager->create_database("testdb");
  auto db = db_manager->open_database("testdb");

  // 创建 users 表
  sql::TableSchema user_schema("users");
  user_schema.add_column("id", sql::DataType::INT, true, false);
  user_schema.add_column("name", sql::DataType::VARCHAR, false, false);
  user_schema.add_column("age", sql::DataType::INT, false, false);
  user_schema.add_column("email", sql::DataType::VARCHAR, true, true);
  db->create_table(user_schema);

  // 插入测试数据
  auto users = db->get_table("users");
  for (int i = 1; i <= 10; ++i) {
    auto row = sql::row()
                   .set("id", i)
                   .set("name", "user" + std::to_string(i))
                   .set("age", 20 + i)
                   .set("email", "user" + std::to_string(i) + "@test.com")
                   .build(user_schema);
    users->insert(row);
  }

  // 4. 执行 SQL
  execute_sql("SELECT * FROM users;", db_manager, "testdb");
  execute_sql("SELECT id, name FROM users WHERE age > 25;", db_manager,
              "testdb");
  execute_sql("SELECT * FROM users WHERE id > 3 AND id < 7;", db_manager,
              "testdb");

  return 0;
}