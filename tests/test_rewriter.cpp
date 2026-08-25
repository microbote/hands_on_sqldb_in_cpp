// test_rewriter.cpp
#include <iostream>
#include <memory>
#include <vector>

#include "rewriter.h"
#include "statement.h"

using namespace sql;

int main() {
  std::cout << "🧪 测试 Query Rewriter" << std::endl;
  std::cout << std::string(50, '=') << std::endl;

  QueryRewriter rewriter;

  // 创建测试条件
  struct TestCase {
    std::string name;
    std::vector<Condition> conditions;
    int expected_count;
  };

  std::vector<TestCase> tests = {
      {"去重测试",
       {{"age", CompareOp::EQ, Value(18)},
        {"age", CompareOp::EQ, Value(18)},
        {"age", CompareOp::EQ, Value(18)}},
       1},
      {"范围合并 - GT",
       {{"age", CompareOp::GT, Value(10)}, {"age", CompareOp::GT, Value(20)}},
       1},
      {"范围合并 - LT",
       {{"age", CompareOp::LT, Value(30)}, {"age", CompareOp::LT, Value(25)}},
       1},
      {"混合条件",
       {{"age", CompareOp::GT, Value(10)},
        {"age", CompareOp::LT, Value(30)},
        {"name", CompareOp::EQ, Value("Alice")}},
       3}};

  for (const auto& test : tests) {
    std::cout << "\n测试: " << test.name << std::endl;
    std::cout << "  输入条件数: " << test.conditions.size() << std::endl;

    auto rewritten = rewriter.rewrite_conditions(test.conditions);
    std::cout << "  输出条件数: " << rewritten.size() << std::endl;

    if (rewritten.size() == test.expected_count) {
      std::cout << "  ✅ 通过" << std::endl;
    } else {
      std::cout << "  ❌ 失败 (期望: " << test.expected_count << ")"
                << std::endl;
    }
  }

  return 0;
}
