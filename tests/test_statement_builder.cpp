// test_statement_builder.cpp
#include <iostream>
#include <memory>

#include "ast.h"
#include "mock_engine.h"
#include "statement_builder.h"

extern int yyparse();
extern void yy_scan_string(const char* str);
extern void yy_delete_buffer(YY_BUFFER_STATE buffer);
extern YY_BUFFER_STATE yy_scan_string(const char* str);
extern ASTNode* g_parsed_ast;

int main() {
  std::cout << "🧪 测试 Statement Builder" << std::endl;
  std::cout << std::string(50, '=') << std::endl;

  auto engine = storage::StorageEngineFactory::create_mock_engine();
  sql::StatementBuilder builder(engine);

  // 创建测试表
  storage::TableSchema schema;
  schema.table_name = "users";
  schema.columns = {{"id", storage::DataType::INTEGER},
                    {"name", storage::DataType::STRING},
                    {"age", storage::DataType::INTEGER}};
  engine->create_table(schema);

  std::vector<std::string> tests = {
      "SELECT * FROM users;", "SELECT id, name FROM users WHERE age > 18;",
      "INSERT INTO users (id, name, age) VALUES (1, 'Alice', 30);",
      "UPDATE users SET age = 31 WHERE id = 1;",
      "DELETE FROM users WHERE id = 1;",
      // 错误测试
      "SELECT * FROM non_existent;", "SELECT invalid_col FROM users;"};

  for (const auto& sql : tests) {
    std::cout << "\nSQL: " << sql << std::endl;

    YY_BUFFER_STATE buffer = yy_scan_string(sql.c_str());
    int result = yyparse();
    yy_delete_buffer(buffer);

    if (result != 0 || !g_parsed_ast) {
      std::cout << "  ❌ 解析失败" << std::endl;
      continue;
    }

    auto stmt = builder.build(g_parsed_ast);
    if (stmt) {
      std::cout << "  ✅ " << stmt->to_string() << std::endl;
    } else {
      std::cout << "  ❌ " << builder.get_error() << std::endl;
    }

    free_ast(g_parsed_ast);
    g_parsed_ast = NULL;
  }

  return 0;
}