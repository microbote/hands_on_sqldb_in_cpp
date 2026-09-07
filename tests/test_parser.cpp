// tests/test_parser.cpp
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <vector>
#include <string>
#include <iostream>

#include "parser/ast.h"
#include "parser/parser.tab.h"
#include "parser/lex.yy.h"
#include "parser/parser.h"

using namespace std;

extern int yyparse();
extern void yy_delete_buffer(YY_BUFFER_STATE buffer);
extern YY_BUFFER_STATE yy_scan_string(const char* str);
extern ASTNode* g_parsed_ast;

/* ============================================================
   测试统计
   ============================================================ */
struct TestStats {
    int total = 0;
    int passed = 0;
    int failed = 0;
    vector<string> failures;
} g_stats;

/* ============================================================
   辅助函数
   ============================================================ */
void print_separator(const char* title = nullptr) {
    printf("\n");
    for (int i = 0; i < 60; i++) printf("=");
    printf("\n");
    if (title) {
        printf("  %s\n", title);
        for (int i = 0; i < 60; i++) printf("=");
        printf("\n");
    }
}

void print_indent(int indent) {
    for (int i = 0; i < indent; i++) printf("  ");
}

/* ============================================================
   测试单个 SQL
   ============================================================ */
bool test_sql(const char* sql, const char* description, bool expect_success = true) {
    g_stats.total++;
    
    print_separator();
    printf("📝 测试: %s\n", description);
    printf("SQL: %s\n", sql);
    printf("\n");

    // 解析 SQL
    YY_BUFFER_STATE buffer = yy_scan_string(sql);
    int result = yyparse();
    yy_delete_buffer(buffer);

    bool success = (result == 0 && g_parsed_ast != nullptr);

    if (success == expect_success) {
        printf("✅ 解析%s\n", expect_success ? "成功！" : "失败（符合预期）");
        g_stats.passed++;
        
        if (success && g_parsed_ast) {
            printf("\n📊 AST 结构:\n");
            print_ast(g_parsed_ast, 2);
            
            // 验证 AST 不为空
            if (g_parsed_ast->type < 0 || g_parsed_ast->type >= NODE_TYPE_COUNT) {
                printf("⚠️  警告: 无效的节点类型 %d\n", g_parsed_ast->type);
            }
        }
        
        reset_parser();
        return true;
    } else {
        printf("❌ 解析%s (预期: %s)\n", 
               success ? "成功" : "失败",
               expect_success ? "成功" : "失败");
        g_stats.failed++;
        g_stats.failures.push_back(string(sql) + " (" + string(description) + ")");
        
        if (g_parsed_ast) {
            reset_parser();
        }
        return false;
    }
}

/* ============================================================
   测试：基础 SQL
   ============================================================ */
void test_basic_sql() {
    print_separator("基础 SQL 测试");
    
    // USE 语句
    test_sql("USE testdb;", "USE 语句");
    
    // SELECT 语句
    test_sql("SELECT * FROM users;", "SELECT *");
    test_sql("SELECT id, name FROM users;", "SELECT 多列");
    test_sql("SELECT id, name, age FROM users;", "SELECT 三列");
    
    // SELECT 带 WHERE
    test_sql("SELECT * FROM users WHERE age > 18;", "SELECT 带 WHERE");
    test_sql("SELECT id, name FROM users WHERE age >= 18;", "SELECT 带 WHERE (>=)");
    test_sql("SELECT * FROM users WHERE age < 65;", "SELECT 带 WHERE (<)");
    test_sql("SELECT * FROM users WHERE age <= 65;", "SELECT 带 WHERE (<=)");
    test_sql("SELECT * FROM users WHERE age != 18;", "SELECT 带 WHERE (!=)");
    test_sql("SELECT * FROM users WHERE name = 'Alice';", "SELECT 带字符串比较");
}

/* ============================================================
   测试：复杂条件
   ============================================================ */
void test_complex_conditions() {
    print_separator("复杂条件测试");
    
    // AND/OR 组合
    test_sql("SELECT * FROM users WHERE age > 18 AND status = 'active';", 
             "AND 条件");
    test_sql("SELECT * FROM users WHERE age > 18 OR status = 'inactive';", 
             "OR 条件");
    test_sql("SELECT * FROM users WHERE age > 18 AND age < 65;", 
             "范围条件 (AND)");
    
    // 括号分组
    test_sql("SELECT * FROM users WHERE (age > 18);", 
             "简单括号");
    test_sql("SELECT * FROM users WHERE age > 18 AND (status = 'active' OR status = 'pending');", 
             "AND + 括号");
    test_sql("SELECT * FROM users WHERE (age > 18 OR age < 10) AND status = 'active';", 
             "OR + 括号");
    
    // 嵌套条件
    test_sql("SELECT * FROM products WHERE price > 100 AND (category = 'Electronics' OR category = 'Computers');", 
             "嵌套条件");
    test_sql("SELECT * FROM products WHERE (price > 100 AND (category = 'Electronics' OR category = 'Computers'));", 
             "多层嵌套");
}

/* ============================================================
   测试：IN 和 LIKE
   ============================================================ */
void test_in_like() {
    print_separator("IN 和 LIKE 测试");
    
    // IN
    test_sql("SELECT * FROM users WHERE id IN (1, 2, 3);", "IN 单列");
    test_sql("SELECT * FROM users WHERE id NOT IN (1, 2, 3);", "NOT IN");
    test_sql("SELECT * FROM users WHERE id IN (1, 2, 3, 4, 5);", "IN 多值");
    
    // LIKE
    test_sql("SELECT * FROM users WHERE name LIKE 'John%';", "LIKE 前缀");
    test_sql("SELECT * FROM users WHERE name LIKE '%son';", "LIKE 后缀");
    test_sql("SELECT * FROM users WHERE name LIKE '%a%';", "LIKE 包含");
    test_sql("SELECT * FROM users WHERE name NOT LIKE 'admin%';", "NOT LIKE");
}

/* ============================================================
   测试：IS NULL / IS NOT NULL
   ============================================================ */
void test_is_null() {
    print_separator("IS NULL / IS NOT NULL 测试");
    
    test_sql("SELECT * FROM users WHERE email IS NULL;", "IS NULL");
    test_sql("SELECT * FROM users WHERE email IS NOT NULL;", "IS NOT NULL");
    test_sql("SELECT * FROM users WHERE age IS NULL AND name IS NOT NULL;", 
             "IS NULL + IS NOT NULL");
}

/* ============================================================
   测试：ORDER BY 和 LIMIT
   ============================================================ */
void test_order_limit() {
    print_separator("ORDER BY 和 LIMIT 测试");
    
    // ORDER BY
    test_sql("SELECT * FROM users ORDER BY id;", "ORDER BY (默认 ASC)");
    test_sql("SELECT * FROM users ORDER BY id ASC;", "ORDER BY ASC");
    test_sql("SELECT * FROM users ORDER BY id DESC;", "ORDER BY DESC");
    test_sql("SELECT * FROM users ORDER BY name DESC;", "ORDER BY 字符串");
    
    // ORDER BY 多列
    test_sql("SELECT * FROM users ORDER BY age DESC, name ASC;", "ORDER BY 多列");
    
    // LIMIT
    test_sql("SELECT * FROM users LIMIT 10;", "LIMIT");
    test_sql("SELECT * FROM users LIMIT 10 OFFSET 20;", "LIMIT + OFFSET");
    test_sql("SELECT * FROM users ORDER BY id LIMIT 5 OFFSET 10;", "ORDER BY + LIMIT + OFFSET");
}

/* ============================================================
   测试：INSERT
   ============================================================ */
void test_insert() {
    print_separator("INSERT 测试");
    
    test_sql("INSERT INTO users (id, name) VALUES (1, 'Alice');", 
             "INSERT 两列");
    test_sql("INSERT INTO users (id, name, age) VALUES (1, 'Bob', 25);", 
             "INSERT 三列");
    test_sql("INSERT INTO users (id, name, age, email) VALUES (1, 'Charlie', 30, 'charlie@test.com');", 
             "INSERT 四列");
    test_sql("INSERT INTO users (name) VALUES ('Alice');", 
             "INSERT 单列（自动生成 ID）");
    test_sql("INSERT INTO users VALUES (1, 'David', 28, 'david@test.com');", 
             "INSERT 无列名");
}

/* ============================================================
   测试：UPDATE
   ============================================================ */
void test_update() {
    print_separator("UPDATE 测试");
    
    test_sql("UPDATE users SET name = 'Jane' WHERE id = 1;", 
             "UPDATE 单列");
    test_sql("UPDATE users SET name = 'Jane', age = 30 WHERE id = 1;", 
             "UPDATE 多列");
    test_sql("UPDATE users SET age = age + 1 WHERE age < 18;", 
             "UPDATE 表达式（简单）");
    test_sql("UPDATE users SET status = 'inactive' WHERE age > 65;", 
             "UPDATE 带条件");
    test_sql("UPDATE users SET name = 'Admin' WHERE id IN (1, 2, 3);", 
             "UPDATE + IN");
}

/* ============================================================
   测试：DELETE
   ============================================================ */
void test_delete() {
    print_separator("DELETE 测试");
    
    test_sql("DELETE FROM users WHERE id = 1;", 
             "DELETE 单行");
    test_sql("DELETE FROM users WHERE age > 65;", 
             "DELETE 多行");
    test_sql("DELETE FROM users WHERE name LIKE '%test%';", 
             "DELETE + LIKE");
    test_sql("DELETE FROM users WHERE id IN (1, 2, 3);", 
             "DELETE + IN");
    test_sql("DELETE FROM users WHERE age IS NULL;", 
             "DELETE + IS NULL");
}

/* ============================================================
   测试：DDL
   ============================================================ */
void test_ddl() {
    print_separator("DDL 测试");
    
    // DATABASE
    test_sql("USE testdb;", "USE DATABASE");
    test_sql("CREATE DATABASE testdb;", "CREATE DATABASE");
    test_sql("DROP DATABASE testdb;", "DROP DATABASE");
    
    // TABLE
    test_sql("CREATE TABLE users (id INT PRIMARY KEY, name VARCHAR);", 
             "CREATE TABLE 简单");
    test_sql("CREATE TABLE users (id INT PRIMARY KEY NOT NULL, name VARCHAR NOT NULL, age INT);", 
             "CREATE TABLE NOT NULL");
    test_sql("CREATE TABLE users (id INT PRIMARY KEY, name VARCHAR NOT NULL, age INT NULL);", 
             "CREATE TABLE 混合约束");
    test_sql("DROP TABLE users;", "DROP TABLE");
}

/* ============================================================
   测试：错误 SQL（预期失败）
   ============================================================ */
void test_error_sql() {
    print_separator("错误 SQL 测试（预期失败）");
    
    // 语法错误
    test_sql("SELEC * FROM users;", "拼写错误 (SELEC)", false);
    test_sql("SELECT * FROM;", "缺少表名", false);
    test_sql("SELECT * users;", "缺少 FROM", false);
    test_sql("SELECT * FROM users WHERE;", "WHERE 缺少条件", false);
    test_sql("INSERT INTO users VALUES (1, 'Alice');", "缺少列定义", true); // 是合法的
    
    // 这里可以根据实际需要添加更多错误测试
}

/* ============================================================
   测试：混合复杂 SQL
   ============================================================ */
void test_complex_sql() {
    print_separator("复杂综合 SQL 测试");
    
    // 复杂的 SELECT
    test_sql(
        "SELECT id, name, age FROM users "
        "WHERE age > 18 AND status = 'active' AND email IS NOT NULL "
        "ORDER BY age DESC, name ASC "
        "LIMIT 10 OFFSET 20;",
        "复杂 SELECT（全功能）"
    );
    
    // 带子查询风格的 IN（目前不支持子查询）
    test_sql(
        "SELECT * FROM users WHERE id IN (1, 2, 3, 4, 5, 6, 7, 8, 9, 10);",
        "长 IN 列表"
    );
    
    // 嵌套括号
    test_sql(
        "SELECT * FROM products WHERE (price > 100 OR price < 10) AND (category = 'Electronics' OR category = 'Books');",
        "复杂嵌套括号"
    );
    
    // 所有特性组合
    test_sql(
        "SELECT id, name, age, email FROM users "
        "WHERE (age > 18 OR age < 10) AND email IS NOT NULL AND name LIKE 'A%' "
        "ORDER BY age DESC "
        "LIMIT 5 OFFSET 0;",
        "所有特性组合"
    );
}

/* ============================================================
   测试：AST 内存管理
   ============================================================ */
void test_memory_management() {
    print_separator("内存管理测试");
    
    // 连续解析多个 SQL，测试 reset_parser 是否正确释放
    const char* sqls[] = {
        "SELECT * FROM users;",
        "SELECT id, name FROM users WHERE age > 18;",
        "INSERT INTO users (id, name) VALUES (1, 'Alice');",
        "UPDATE users SET name = 'Bob' WHERE id = 1;",
        "DELETE FROM users WHERE id = 1;",
    };
    
    int count = 0;
    for (const char* sql : sqls) {
        count++;
        printf("\n--- 连续解析 #%d: %s ---\n", count, sql);
        
        YY_BUFFER_STATE buffer = yy_scan_string(sql);
        int result = yyparse();
        yy_delete_buffer(buffer);
        
        if (result == 0 && g_parsed_ast) {
            printf("✅ 解析成功\n");
            // 打印 AST 摘要
            printf("  AST 类型: %s\n", node_type_to_string(g_parsed_ast->type));
            reset_parser();
        } else {
            printf("❌ 解析失败\n");
            if (g_parsed_ast) reset_parser();
        }
    }
}

void test_parser() {
  parser::Parser parser;
  auto result = parser.parse("SELECT * FROM users;");
  
  if (result) {
      // 方式1：使用 -> 访问
      std::cout << "AST type: " << node_type_to_string(result->type) << std::endl;
      
      // 方式2：使用 get() 获取裸指针
      ASTNode* node = result.get();
      
      print_ast(node, 2);
      // 方式3：释放所有权（如果需要在外部管理）
      // ASTNode* raw = result.release();
      // 注意：释放后需要手动 free
  }
  
  // result 析构时自动 free_ast
}

void test_vector_of_results() {
  parser::Parser parser;
  auto results = parser.parse_multi(
      "SELECT * FROM users; \n"
      "SELECT id FROM users; \n"
      "INSERT INTO users VALUES (1, 'Alice');\n"
  );
  
  for (auto& r : results) {
      if (r) {
          std::cout << "✅ " << node_type_to_string(r->type) 
                    <<":" << r.s_sql() <<'\n';
      }
      // 每个结果析构时自动释放
  }
}

/* ============================================================
   测试总结
   ============================================================ */
void print_summary() {
    print_separator("测试总结");
    
    printf("\n📊 测试结果:\n");
    printf("  总测试数: %d\n", g_stats.total);
    printf("  ✅ 通过: %d\n", g_stats.passed);
    printf("  ❌ 失败: %d\n", g_stats.failed);
    printf("  通过率: %.1f%%\n", 
           g_stats.total > 0 ? (g_stats.passed * 100.0 / g_stats.total) : 0);
    
    if (g_stats.failed > 0) {
        printf("\n❌ 失败列表:\n");
        for (const auto& f : g_stats.failures) {
            printf("  - %s\n", f.c_str());
        }
    }
    
    printf("\n%s\n", g_stats.failed == 0 ? "🎉 所有测试通过！" : "⚠️  有测试失败，请检查。");
    printf("\n");
    for (int i = 0; i < 60; i++) printf("=");
    printf("\n");
}

/* ============================================================
   主函数
   ============================================================ */
int main() {
    printf("╔══════════════════════════════════════════╗\n");
    printf("║     SQL Parser 完整测试套件             ║\n");
    printf("╚══════════════════════════════════════════╝\n");

    // 运行所有测试
    test_basic_sql();
    test_complex_conditions();
    test_in_like();
    test_is_null();
    test_order_limit();
    test_insert();
    test_update();
    test_delete();
    test_ddl();
    test_error_sql();
    test_complex_sql();
    test_memory_management();
    test_parser();
    test_vector_of_results();
    
    // 打印总结
    print_summary();

    return g_stats.failed > 0 ? 1 : 0;
}