// test_middle.cpp
#include <iostream>
#include <memory>
#include <string>

#include "ast.h"
#include "executor.h"
#include "mock_engine.h"
#include "optimizer.h"
#include "statement_builder.h"

// Flex/Bison 外部声明
extern int yyparse();
extern void yy_scan_string(const char* str);
extern void yy_delete_buffer(YY_BUFFER_STATE buffer);
extern YY_BUFFER_STATE yy_scan_string(const char* str);
extern ASTNode* g_parsed_ast;

class MiddleTester {
 public:
  MiddleTester() {
    // 创建 Mock 引擎
    engine_ = storage::StorageEngineFactory::create_mock_engine();
    std::cout << "✅ Mock 存储引擎已创建" << std::endl;

    // 初始化测试数据
    init_test_data();
  }

  ~MiddleTester() { engine_->close(); }

  // 执行单个 SQL 测试
  bool test_sql(const std::string& sql, const std::string& description) {
    std::cout << "\n" << std::string(60, '=') << std::endl;
    std::cout << "📝 测试: " << description << std::endl;
    std::cout << "SQL: " << sql << std::endl;
    std::cout << std::string(60, '=') << std::endl;

    // 1. 词法/语法分析
    YY_BUFFER_STATE buffer = yy_scan_string(sql.c_str());
    int parse_result = yyparse();
    yy_delete_buffer(buffer);

    if (parse_result != 0 || !g_parsed_ast) {
      std::cout << "❌ 语法解析失败" << std::endl;
      return false;
    }
    std::cout << "✅ 语法解析成功" << std::endl;

    // 打印 AST
    std::cout << "\n📊 AST 结构:" << std::endl;
    print_ast(g_parsed_ast, 2);

    // 2. 构建 + 验证 (中端第一步)
    sql::StatementBuilder builder(engine_);
    auto stmt = builder.build(g_parsed_ast);
    if (!stmt) {
      std::cout << "❌ 构建/验证失败: " << builder.get_error() << std::endl;
      free_ast(g_parsed_ast);
      g_parsed_ast = NULL;
      return false;
    }
    std::cout << "\n✅ Statement 构建成功:" << std::endl;
    std::cout << "   " << stmt->to_string() << std::endl;

    // 3. 优化 (中端第二步)
    sql::Optimizer optimizer(engine_);
    auto plan = optimizer.optimize(stmt.get());
    if (!plan) {
      std::cout << "❌ 优化失败" << std::endl;
      free_ast(g_parsed_ast);
      g_parsed_ast = NULL;
      return false;
    }
    std::cout << "✅ 执行计划生成成功:" << std::endl;
    std::cout << "   " << plan->to_string() << std::endl;
    std::cout << "   估算代价: " << plan->estimated_cost.total() << std::endl;

    // 4. 执行 (中端第三步)
    sql::Executor executor(engine_);
    auto result = executor.execute(plan.get());

    if (result.success) {
      std::cout << "\n✅ 执行成功: " << result.message << std::endl;
      if (result.affected_rows > 0) {
        std::cout << "   受影响行数: " << result.affected_rows << std::endl;
      }
    } else {
      std::cout << "\n❌ 执行失败: " << result.message << std::endl;
    }

    // 打印表状态
    if (stmt->type() == sql::StatementType::SELECT) {
      auto select_stmt = static_cast<sql::SelectStatement*>(stmt.get());
      engine_->get_row_count(select_stmt->table_name);
    }

    free_ast(g_parsed_ast);
    g_parsed_ast = NULL;

    return result.success;
  }

  // 运行所有测试
  void run_all_tests() {
    std::cout << "\n" << std::string(60, '=') << std::endl;
    std::cout << "🧪 开始运行中端完整测试套件" << std::endl;
    std::cout << std::string(60, '=') << std::endl;

    int passed = 0;
    int total = 0;

    // 测试用例
    struct TestCase {
      std::string sql;
      std::string description;
      bool expected;
    };

    std::vector<TestCase> tests = {
        // 基本查询
        {"SELECT * FROM users;", "SELECT * (全表扫描)", true},
        {"SELECT id, name FROM users;", "SELECT 指定列", true},
        {"SELECT id, name, age FROM users WHERE age > 22;",
         "SELECT 带条件 (> )", true},
        {"SELECT id, name, age FROM users WHERE age > 20 AND age < 25;",
         "SELECT 多条件 (AND)", true},
        {"SELECT id, name FROM users WHERE age > 25 OR name = 'user2';",
         "SELECT 多条件 (OR)", true},
        {"SELECT * FROM users WHERE age >= 20 AND age <= 25;",
         "SELECT 范围条件", true},
        {"SELECT * FROM users WHERE name LIKE 'user%';", "SELECT LIKE 查询",
         true},

        // 插入操作
        {"INSERT INTO users (id, name, age, email) VALUES (11, 'Alice', 30, "
         "'alice@test.com');",
         "INSERT 指定列", true},
        {"INSERT INTO users (name, age, email) VALUES ('Bob', 28, "
         "'bob@test.com');",
         "INSERT 自动生成主键", true},
        {"INSERT INTO users (id, name, age, email) VALUES (13, 'Charlie', 35, "
         "'charlie@test.com');",
         "INSERT 所有列", true},

        // 更新操作
        {"UPDATE users SET age = 31 WHERE id = 11;", "UPDATE 单列", true},
        {"UPDATE users SET age = 29, email = 'bob_new@test.com' WHERE name = "
         "'Bob';",
         "UPDATE 多列", true},
        {"UPDATE users SET age = age + 1 WHERE age < 25;",
         "UPDATE 表达式 (简单)", true},

        // 删除操作
        {"DELETE FROM users WHERE id = 13;", "DELETE 单行", true},
        {"DELETE FROM users WHERE age > 30;", "DELETE 多行", true},

        // USE 语句
        {"USE testdb;", "USE 语句", true},

        // 验证查询
        {"SELECT * FROM users;", "验证最终结果", true},
        {"SELECT COUNT(*) FROM users;", "聚合查询 (暂不支持)",
         false},  // 预期失败

        // 错误测试
        {"SELECT * FROM non_existent_table;", "表不存在 (应报错)", false},
        {"SELECT invalid_column FROM users;", "列不存在 (应报错)", false},
        {"INSERT INTO users (invalid_col) VALUES (1);", "插入无效列 (应报错)",
         false},
        {"INSERT INTO users (id, name) VALUES (1);",
         "列数和值数不匹配 (应报错)", false},
        {"UPDATE users SET invalid_col = 1 WHERE id = 1;",
         "更新无效列 (应报错)", false},
        {"DELETE FROM users WHERE invalid_col > 1;", "删除无效列条件 (应报错)",
         false},
    };

    for (const auto& test : tests) {
      total++;
      bool result = test_sql(test.sql, test.description);
      bool success = (result == test.expected);

      if (success) {
        passed++;
        std::cout << "✅ 测试通过\n" << std::endl;
      } else {
        std::cout << "❌ 测试失败 (期望: " << (test.expected ? "成功" : "失败")
                  << ", 实际: " << (result ? "成功" : "失败") << ")\n"
                  << std::endl;
      }
    }

    // 打印测试报告
    std::cout << "\n" << std::string(60, '=') << std::endl;
    std::cout << "📊 测试报告" << std::endl;
    std::cout << std::string(60, '=') << std::endl;
    std::cout << "总测试数: " << total << std::endl;
    std::cout << "通过: " << passed << std::endl;
    std::cout << "失败: " << (total - passed) << std::endl;
    std::cout << "通过率: " << (passed * 100.0 / total) << "%" << std::endl;
    std::cout << std::string(60, '=') << std::endl;

    // 打印最终表状态
    std::cout << "\n📋 最终表状态:" << std::endl;
    engine_->dump_table("users");
  }

 private:
  std::shared_ptr<storage::StorageEngine> engine_;

  void init_test_data() {
    // 创建 users 表
    storage::TableSchema schema;
    schema.table_name = "users";
    schema.columns = {{"id", storage::DataType::INTEGER},
                      {"name", storage::DataType::STRING},
                      {"age", storage::DataType::INTEGER},
                      {"email", storage::DataType::STRING}};
    schema.primary_key_index = 0;  // id 是主键

    if (!engine_->create_table(schema)) {
      std::cout << "⚠️  users 表已存在，跳过创建" << std::endl;
      return;
    }

    std::cout << "✅ 创建表 users" << std::endl;

    // 插入测试数据
    std::vector<storage::Row> test_data = {
        {storage::Value(1), storage::Value("user1"), storage::Value(20),
         storage::Value("user1@test.com")},
        {storage::Value(2), storage::Value("user2"), storage::Value(22),
         storage::Value("user2@test.com")},
        {storage::Value(3), storage::Value("user3"), storage::Value(23),
         storage::Value("user3@test.com")},
        {storage::Value(4), storage::Value("user4"), storage::Value(24),
         storage::Value("user4@test.com")},
        {storage::Value(5), storage::Value("user5"), storage::Value(25),
         storage::Value("user5@test.com")},
        {storage::Value(6), storage::Value("user6"), storage::Value(26),
         storage::Value("user6@test.com")},
        {storage::Value(7), storage::Value("user7"), storage::Value(27),
         storage::Value("user7@test.com")},
        {storage::Value(8), storage::Value("user8"), storage::Value(28),
         storage::Value("user8@test.com")},
        {storage::Value(9), storage::Value("user9"), storage::Value(29),
         storage::Value("user9@test.com")},
        {storage::Value(10), storage::Value("user10"), storage::Value(30),
         storage::Value("user10@test.com")}};

    for (const auto& row : test_data) {
      engine_->insert("users", row);
    }

    std::cout << "✅ 插入 10 行测试数据" << std::endl;

    // 显示初始数据
    engine_->dump_table("users");
  }
};

int main() {
  std::cout << "╔══════════════════════════════════════════╗" << std::endl;
  std::cout << "║     SQL 中端完整测试                     ║" << std::endl;
  std::cout << "║     (Statement → Plan → Executor)        ║" << std::endl;
  std::cout << "╚══════════════════════════════════════════╝" << std::endl;

  MiddleTester tester;
  tester.run_all_tests();

  std::cout << "\n👋 测试完成" << std::endl;
  return 0;
}