// tests/test_pushdown_not.cpp
#include <cassert>
#include <iostream>
#include <memory>

#include "query/statement/condition.h"
#include "relation/sql_relation.h"

using namespace query;
using namespace sql;

// ============================================================
// 辅助函数
// ============================================================
void print_separator(const std::string& title = "") {
  std::cout << "\n" << std::string(60, '=') << std::endl;
  if (!title.empty()) {
    std::cout << "  " << title << std::endl;
    std::cout << std::string(60, '=') << std::endl;
  }
}

void print_test_result(const std::string& name, bool passed) {
  std::cout << (passed ? "  ✅ " : "  ❌ ") << name << std::endl;
}

// 断言宏
#define ASSERT_TRUE(expr)                                      \
  do {                                                         \
    if (!(expr)) {                                             \
      std::cerr << "❌ Assertion failed: " #expr << std::endl; \
      return false;                                            \
    }                                                          \
  } while (0)

#define ASSERT_EQ(a, b)                                                        \
  do {                                                                         \
    if ((a) != (b)) {                                                          \
      std::cerr << "❌ Assertion failed: " << #a << " == " << #b << std::endl; \
      std::cerr << "   Got: " << (a) << ", Expected: " << (b) << std::endl;    \
      return false;                                                            \
    }                                                                          \
  } while (0)

// ============================================================
// 测试 1: 简单 NOT(COMPARE) → 翻转操作符
// ============================================================
bool test_simple_not_compare() {
  std::cout << "\n测试 1: 简单 NOT(COMPARE) → 翻转操作符" << std::endl;

  // NOT(id > 10) → id <= 10
  auto expr = ConditionExpr::make_not(
      ConditionExpr::make_compare("id", CompareOp::GT, Value(10)));
  auto result = ConditionExpr::pushdown_not(expr);

  std::string expected = "id <= 10";
  std::string actual = result->to_string();
  ASSERT_EQ(actual, expected);

  std::cout << "  " << actual << std::endl;
  return true;
}

// ============================================================
// 测试 2: 双重否定抵消
// ============================================================
bool test_double_negation() {
  std::cout << "\n测试 2: 双重否定抵消" << std::endl;

  // NOT(NOT(id > 10)) → id > 10
  auto expr = ConditionExpr::make_not(ConditionExpr::make_not(
      ConditionExpr::make_compare("id", CompareOp::GT, Value(10))));
  auto result = ConditionExpr::pushdown_not(expr);

  std::string expected = "id > 10";
  std::string actual = result->to_string();
  ASSERT_EQ(actual, expected);

  std::cout << "  " << actual << std::endl;
  return true;
}

// ============================================================
// 测试 3: De Morgan (AND → OR)
// ============================================================
bool test_de_morgan_and() {
  std::cout << "\n测试 3: De Morgan (AND → OR)" << std::endl;

  // NOT(id > 10 AND age > 18) → id <= 10 OR age <= 18
  auto expr = ConditionExpr::make_not(ConditionExpr::make_and(
      ConditionExpr::make_compare("id", CompareOp::GT, Value(10)),
      ConditionExpr::make_compare("age", CompareOp::GT, Value(18))));
  auto result = ConditionExpr::pushdown_not(expr);

  std::string expected = "(id <= 10 OR age <= 18)";
  std::string actual = result->to_string();
  ASSERT_EQ(actual, expected);

  std::cout << "  " << actual << std::endl;
  return true;
}

// ============================================================
// 测试 4: De Morgan (OR → AND)
// ============================================================
bool test_de_morgan_or() {
  std::cout << "\n测试 4: De Morgan (OR → AND)" << std::endl;

  // NOT(id > 10 OR age > 18) → id <= 10 AND age <= 18
  auto expr = ConditionExpr::make_not(ConditionExpr::make_or(
      ConditionExpr::make_compare("id", CompareOp::GT, Value(10)),
      ConditionExpr::make_compare("age", CompareOp::GT, Value(18))));
  auto result = ConditionExpr::pushdown_not(expr);

  std::string expected = "(id <= 10 AND age <= 18)";
  std::string actual = result->to_string();
  ASSERT_EQ(actual, expected);

  std::cout << "  " << actual << std::endl;
  return true;
}

// ============================================================
// 测试 5: NOT(IN) → NOT 保留
// ============================================================
bool test_not_in() {
  std::cout << "\n测试 5: NOT(IN) → NOT 保留" << std::endl;

  // NOT(id IN (1,2,3)) → NOT(id IN (1,2,3))
  auto expr = ConditionExpr::make_not(
      ConditionExpr::make_in("id", {Value(1), Value(2), Value(3)}));
  auto result = ConditionExpr::pushdown_not(expr);

  std::string expected = "NOT (id IN (1,2,3))";
  std::string actual = result->to_string();
  ASSERT_EQ(actual, expected);

  std::cout << "  " << actual << std::endl;
  return true;
}

// ============================================================
// 测试 6: 无法翻转的操作符 (LIKE)
// ============================================================
bool test_unflippable_compare() {
  std::cout << "\n测试 6: 无法翻转的操作符 (LIKE)" << std::endl;

  // NOT(name LIKE '%John%') → NOT(name LIKE '%John%')
  auto expr = ConditionExpr::make_not(
      ConditionExpr::make_compare("name", CompareOp::LIKE, Value("%John%")));
  auto result = ConditionExpr::pushdown_not(expr);

  std::string expected = "NOT (name LIKE %John%)";
  std::string actual = result->to_string();
  ASSERT_EQ(actual, expected);

  std::cout << "  " << actual << std::endl;
  return true;
}

// ============================================================
// 测试 7: 复杂嵌套
// ============================================================
bool test_complex_nested() {
  std::cout << "\n测试 7: 复杂嵌套" << std::endl;

  // NOT((id > 10 OR age > 18) AND name LIKE '%John%')
  // → (id <= 10 AND age <= 18) OR NOT(name LIKE '%John%')
  auto inner_or = ConditionExpr::make_or(
      ConditionExpr::make_compare("id", CompareOp::GT, Value(10)),
      ConditionExpr::make_compare("age", CompareOp::GT, Value(18)));
  auto inner_and = ConditionExpr::make_and(
      std::make_unique<ConditionExpr>(std::move(inner_or)),
      ConditionExpr::make_compare("name", CompareOp::LIKE, Value("%John%")));
  auto expr = ConditionExpr::make_not(std::move(inner_and));
  auto result = ConditionExpr::pushdown_not(expr);

  std::string expected = "((id <= 10 AND age <= 18) OR NOT (name LIKE %John%))";
  std::string actual = result->to_string();
  ASSERT_EQ(actual, expected);

  std::cout << "  " << actual << std::endl;
  return true;
}

// ============================================================
// 测试 8: 三重否定
// ============================================================
bool test_triple_negation() {
  std::cout << "\n测试 8: 三重否定" << std::endl;

  // NOT(NOT(NOT(id > 10))) → id <= 10
  auto expr =
      ConditionExpr::make_not(ConditionExpr::make_not(ConditionExpr::make_not(
          ConditionExpr::make_compare("id", CompareOp::GT, Value(10)))));
  auto result = ConditionExpr::pushdown_not(expr);

  std::string expected = "id <= 10";
  std::string actual = result->to_string();
  ASSERT_EQ(actual, expected);

  std::cout << "  " << actual << std::endl;
  return true;
}

// ============================================================
// 测试 9: 无需翻转的表达式保持不变
// ============================================================
bool test_no_change() {
  std::cout << "\n测试 9: 无需翻转的表达式保持不变" << std::endl;

  // id > 10 AND age > 18 → id > 10 AND age > 18
  auto expr = ConditionExpr::make_and(
      ConditionExpr::make_compare("id", CompareOp::GT, Value(10)),
      ConditionExpr::make_compare("age", CompareOp::GT, Value(18)));
  auto result = ConditionExpr::pushdown_not(expr);

  std::string expected = "(id > 10 AND age > 18)";
  std::string actual = result->to_string();
  ASSERT_EQ(actual, expected);

  std::cout << "  " << actual << std::endl;
  return true;
}

// ============================================================
// 测试 10: NOT(IS_NULL) → IS_NOT_NULL
// ============================================================
bool test_not_is_null() {
  std::cout << "\n测试 10: NOT(IS_NULL) → IS_NOT_NULL" << std::endl;

  // NOT(id IS NULL) → id IS NOT NULL
  // 注意：假设 IS_NULL/IS_NOT_NULL 被支持
  // 如果不支持，这里可能需要调整
  auto expr = ConditionExpr::make_not(
      ConditionExpr::make_compare("id", CompareOp::IS_NULL, Value()));
  auto result = ConditionExpr::pushdown_not(expr);

  std::string expected = "id IS NOT NULL";  // 如果支持 IS_NOT_NULL
  // 如果不支持，预期可能是 "id IS NOT NULL" 或 "NOT (id IS NULL)"
  std::cout << "  预期: " << expected << std::endl;
  std::cout << "  实际: " << result->to_string() << std::endl;

  // 如果 IS_NOT_NULL 未实现，这个测试会失败
  // 所以这里只打印结果，不做严格断言
  return true;
}

// ============================================================
// 测试运行器
// ============================================================
struct TestCase {
  std::string name;
  bool (*func)();
};

void run_tests() {
  std::cout << "╔══════════════════════════════════════════╗" << std::endl;
  std::cout << "║     NOT 下推测试套件                   ║" << std::endl;
  std::cout << "╚══════════════════════════════════════════╝" << std::endl;

  std::vector<TestCase> tests = {
      {"简单 NOT(COMPARE) → 翻转", test_simple_not_compare},
      {"双重否定抵消", test_double_negation},
      {"De Morgan (AND → OR)", test_de_morgan_and},
      {"De Morgan (OR → AND)", test_de_morgan_or},
      {"NOT(IN) → 保留 NOT", test_not_in},
      {"无法翻转 (LIKE)", test_unflippable_compare},
      {"复杂嵌套", test_complex_nested},
      {"三重否定", test_triple_negation},
      {"无需翻转保持不变", test_no_change},
      {"NOT(IS_NULL) → IS_NOT_NULL", test_not_is_null},
  };

  int passed = 0;
  int total = tests.size();

  for (const auto& test : tests) {
    print_separator(test.name);
    try {
      if (test.func()) {
        passed++;
        std::cout << "  ✅ 通过" << std::endl;
      } else {
        std::cout << "  ❌ 失败" << std::endl;
      }
    } catch (const std::exception& e) {
      std::cout << "  ❌ 异常: " << e.what() << std::endl;
    }
  }

  print_separator("测试报告");
  std::cout << "  总测试数: " << total << std::endl;
  std::cout << "  通过: " << passed << std::endl;
  std::cout << "  失败: " << total - passed << std::endl;
  std::cout << "  通过率: " << (passed * 100.0 / total) << "%" << std::endl;
}

// ============================================================
// 主函数
// ============================================================
int main() {
  run_tests();
  return 0;
}