// test_parser.cpp
#include <stdio.h>
#include <string.h>

#include "ast.h"
#include "parser.tab.h"
#include "lex.yy.h"
#include <vector>
#include <string>
using namespace std;

extern int yyparse();
extern void yy_delete_buffer(YY_BUFFER_STATE buffer);
extern YY_BUFFER_STATE yy_scan_string(const char* str);

/* 外部 AST 全局变量 */
extern ASTNode* g_parsed_ast;
int g_success_count = 0;
int g_failure_count = 0;
vector<string> g_failures;

void test_sql(const char* sql, const char* description) {
  printf("\n========================================\n");
  printf("测试: %s\n", description);
  printf("SQL: %s\n", sql);
  printf("========================================\n\n");

  // 解析 SQL
  YY_BUFFER_STATE buffer = yy_scan_string(sql);
  int result = yyparse();
  yy_delete_buffer(buffer);

  if (result == 0 && g_parsed_ast) {
    printf("✅ 解析成功！\n\n");
    printf("AST 结构:\n");
    print_ast(g_parsed_ast, 2);
    reset_parser();
    g_success_count++;
  } else {
    printf("❌ 解析失败！\n");
    g_failure_count++;
    g_failures.push_back(sql);
  }
}

void summary() {
  printf("\n========================================\n");
  printf("总结:\n");
  printf("Total: %d\n", g_success_count + g_failure_count);
  printf("成功:%d\n", g_success_count);
  printf("失败:%d\n", g_failure_count);
  if(g_failure_count>0){
    printf("失败 SQL:\n");
    for (auto f : g_failures) {
      printf("%s\n", f.c_str());
    }
  }
  printf("========================================\n");
}

int main() {
  printf("╔══════════════════════════════════════════╗\n");
  printf("║     SQL Parser Test Suite               ║\n");
  printf("╚══════════════════════════════════════════╝\n");

  // 测试1: USE 语句
  test_sql("USE testdb;", "USE 语句");

  // 测试2: SELECT 语句（无条件）
  test_sql("SELECT * FROM users;", "SELECT 无条件");

  // 测试3: SELECT 语句（带条件）
  test_sql("SELECT * FROM users WHERE age > 18;", "SELECT 带条件");

  // 测试4: SELECT 语句（复杂条件）
  test_sql("SELECT * FROM users WHERE age > 18 AND status = 'active';",
           "SELECT 复杂条件");

  // 测试5: INSERT 语句
  test_sql("INSERT INTO users (id, name, age) VALUES (1, 'John', 25);",
           "INSERT 语句");

  // 测试6: UPDATE 语句
  test_sql("UPDATE users SET name = 'Jane', age = 30 WHERE id = 1;",
           "UPDATE 语句");

  // 测试7: DELETE 语句
  test_sql("DELETE FROM users WHERE id = 1;", "DELETE 语句");

  // 测试8: 嵌套条件
  test_sql(
      "SELECT * FROM products WHERE price > 100 AND (category = 'Electronics' "
      "OR category = 'Computers');",
      "SELECT 嵌套条件");

  // 测试9: 空值测试
  test_sql("INSERT INTO users (name) VALUES ('Alice');", "INSERT 单列");

  printf("\n=== 测试嵌套条件 ===\n");

  // 1. 简单括号
  test_sql("SELECT * FROM products WHERE (price > 100);", "简单括号");

  // 2. AND + 括号
  test_sql(
      "SELECT * FROM products WHERE price > 100 AND (category = "
      "'Electronics');",
      "AND + 括号");

  // 3. OR + 括号
  test_sql(
      "SELECT * FROM products WHERE (category = 'Electronics' OR category = "
      "'Computers');",
      "OR + 括号");

  // 4. 复杂的嵌套
  test_sql(
      "SELECT * FROM products WHERE price > 100 AND (category = 'Electronics' "
      "OR category = 'Computers');",
      "复杂嵌套");

  // 5. 多层嵌套
  test_sql(
      "SELECT * FROM products WHERE (price > 100 AND (category = 'Electronics' "
      "OR category = 'Computers'));",
      "多层嵌套");

  // 6. 多个括号
  test_sql(
      "SELECT * FROM products WHERE (price > 100) AND (category = "
      "'Electronics' OR category = 'Computers');",
      "多个括号");

  printf("\n========================================\n");
  printf("所有测试完成！\n");
  printf("========================================\n");
  summary();
  return 0;
}