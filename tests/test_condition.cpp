// tests/test_condition.cpp
#include <iostream>
#include <memory>
#include <vector>

#include "query/statement/condition.h"
#include "relation/sql_relation.h"

using namespace query;
using namespace sql;

void print_separator(const std::string& title = "") {
  std::cout << "\n" << std::string(60, '=') << std::endl;
  if (!title.empty()) {
    std::cout << "  " << title << std::endl;
    std::cout << std::string(60, '=') << std::endl;
  }
}

void print_result(const std::string& test_name, bool passed) {
  std::cout << (passed ? "✅ " : "❌ ") << test_name << std::endl;
}

// ============================================================
// 测试环境准备
// ============================================================
TableSchema create_test_schema() {
  TableSchema schema("users");
  schema.primary_key("id", DataType::INT)
      .not_null("name", DataType::VARCHAR)
      .not_null("age", DataType::INT)
      .nullable("email", DataType::VARCHAR);
  return schema;
}

Row create_test_row(int id, const std::string& name, int age,
                    const std::string& email = "") {
  auto result = row()
                    .set("id", id)
                    .set("name", name)
                    .set("age", age)
                    .set("email", email)
                    .build(create_test_schema());
  return std::move(result.value());
}

// ============================================================
// 测试 1: 构造和 to_string
// ============================================================
void test_construction() {
  print_separator("测试构造和 to_string");

  // 1.1 COMPARE
  auto eq = ConditionExpr::make_compare_expr("id", CompareOp::EQ, Value(1));
  std::cout << "EQ: " << eq.to_string() << std::endl;

  auto gt = ConditionExpr::make_compare_expr("age", CompareOp::GT, Value(18));
  std::cout << "GT: " << gt.to_string() << std::endl;

  auto like = ConditionExpr::make_compare_expr("name", CompareOp::LIKE,
                                               Value("%Alice%"));
  std::cout << "LIKE: " << like.to_string() << std::endl;

  // 1.2 AND
  auto and_expr = ConditionExpr::make_and_expr(
      std::make_unique<ConditionExpr>(eq), std::make_unique<ConditionExpr>(gt));
  std::cout << "AND: " << and_expr.to_string() << std::endl;

  // 1.3 OR
  auto or_expr =
      ConditionExpr::make_or_expr(std::make_unique<ConditionExpr>(eq),
                                  std::make_unique<ConditionExpr>(like));
  std::cout << "OR: " << or_expr.to_string() << std::endl;

  // 1.4 NOT
  auto not_expr =
      ConditionExpr::make_not_expr(std::make_unique<ConditionExpr>(eq));
  std::cout << "NOT: " << not_expr.to_string() << std::endl;

  // 1.5 IN
  auto in_expr =
      ConditionExpr::make_in_expr("id", {Value(1), Value(2), Value(3)});
  std::cout << "IN: " << in_expr.to_string() << std::endl;

  // 1.6 复杂嵌套
  auto complex = ConditionExpr::make_and_expr(
      std::make_unique<ConditionExpr>(ConditionExpr::make_or_expr(
          std::make_unique<ConditionExpr>(eq),
          std::make_unique<ConditionExpr>(in_expr))),
      std::make_unique<ConditionExpr>(gt));
  std::cout << "复杂: " << complex.to_string() << std::endl;
}

// ============================================================
// 测试 2: matches - 行匹配
// ============================================================
void test_matches() {
  print_separator("测试 matches");

  auto schema = create_test_schema();
  auto row = create_test_row(1, "Alice", 25, "alice@example.com");

  // 2.1 EQ 匹配
  auto eq = ConditionExpr::make_compare_expr("id", CompareOp::EQ, Value(1));
  bool result = eq.matches(row, schema);
  print_result("EQ 匹配 (id=1)", result == true);

  // 2.2 EQ 不匹配
  auto eq_false =
      ConditionExpr::make_compare_expr("id", CompareOp::EQ, Value(2));
  result = eq_false.matches(row, schema);
  print_result("EQ 不匹配 (id=2)", result == false);

  // 2.3 GT 匹配
  auto gt = ConditionExpr::make_compare_expr("age", CompareOp::GT, Value(20));
  result = gt.matches(row, schema);
  print_result("GT 匹配 (age>20)", result == true);

  // 2.4 GT 不匹配
  auto gt_false =
      ConditionExpr::make_compare_expr("age", CompareOp::GT, Value(30));
  result = gt_false.matches(row, schema);
  print_result("GT 不匹配 (age>30)", result == false);

  // 2.5 LIKE 匹配（前缀）
  auto like_prefix =
      ConditionExpr::make_compare_expr("name", CompareOp::LIKE, Value("Ali%"));
  result = like_prefix.matches(row, schema);
  print_result("LIKE 前缀匹配 (Ali%)", result == true);

  // 2.6 LIKE 匹配（后缀）
  auto like_suffix =
      ConditionExpr::make_compare_expr("name", CompareOp::LIKE, Value("%ice"));
  result = like_suffix.matches(row, schema);
  print_result("LIKE 后缀匹配 (%ice)", result == true);

  // 2.7 LIKE 匹配（包含）
  auto like_contains =
      ConditionExpr::make_compare_expr("name", CompareOp::LIKE, Value("%lic%"));
  result = like_contains.matches(row, schema);
  print_result("LIKE 包含匹配 (%lic%)", result == true);

  // 2.8 LIKE 不匹配
  auto like_false =
      ConditionExpr::make_compare_expr("name", CompareOp::LIKE, Value("%Bob%"));
  result = like_false.matches(row, schema);
  print_result("LIKE 不匹配 (%Bob%)", result == false);

  // 2.9 AND 匹配
  auto and_true = ConditionExpr::make_and_expr(
      std::make_unique<ConditionExpr>(
          ConditionExpr::make_compare_expr("id", CompareOp::EQ, Value(1))),
      std::make_unique<ConditionExpr>(
          ConditionExpr::make_compare_expr("age", CompareOp::GT, Value(20))));
  result = and_true.matches(row, schema);
  print_result("AND 匹配 (id=1 AND age>20)", result == true);

  // 2.10 AND 不匹配
  auto and_false = ConditionExpr::make_and_expr(
      std::make_unique<ConditionExpr>(
          ConditionExpr::make_compare_expr("id", CompareOp::EQ, Value(1))),
      std::make_unique<ConditionExpr>(
          ConditionExpr::make_compare_expr("age", CompareOp::GT, Value(30))));
  result = and_false.matches(row, schema);
  print_result("AND 不匹配 (id=1 AND age>30)", result == false);

  // 2.11 OR 匹配（左真）
  auto or_true_left = ConditionExpr::make_or_expr(
      std::make_unique<ConditionExpr>(
          ConditionExpr::make_compare_expr("id", CompareOp::EQ, Value(1))),
      std::make_unique<ConditionExpr>(
          ConditionExpr::make_compare_expr("age", CompareOp::GT, Value(30))));
  result = or_true_left.matches(row, schema);
  print_result("OR 匹配 (id=1 OR age>30)", result == true);

  // 2.12 OR 匹配（右真）
  auto or_true_right = ConditionExpr::make_or_expr(
      std::make_unique<ConditionExpr>(
          ConditionExpr::make_compare_expr("id", CompareOp::EQ, Value(5))),
      std::make_unique<ConditionExpr>(
          ConditionExpr::make_compare_expr("age", CompareOp::GT, Value(20))));
  result = or_true_right.matches(row, schema);
  print_result("OR 匹配 (id=5 OR age>20)", result == true);

  // 2.13 OR 不匹配
  auto or_false = ConditionExpr::make_or_expr(
      std::make_unique<ConditionExpr>(
          ConditionExpr::make_compare_expr("id", CompareOp::EQ, Value(5))),
      std::make_unique<ConditionExpr>(
          ConditionExpr::make_compare_expr("age", CompareOp::GT, Value(30))));
  result = or_false.matches(row, schema);
  print_result("OR 不匹配 (id=5 OR age>30)", result == false);

  // 2.14 NOT 匹配
  auto not_true = ConditionExpr::make_not_expr(std::make_unique<ConditionExpr>(
      ConditionExpr::make_compare_expr("id", CompareOp::EQ, Value(2))));
  result = not_true.matches(row, schema);
  print_result("NOT 匹配 (NOT id=2)", result == true);

  // 2.15 NOT 不匹配
  auto not_false = ConditionExpr::make_not_expr(std::make_unique<ConditionExpr>(
      ConditionExpr::make_compare_expr("id", CompareOp::EQ, Value(1))));
  result = not_false.matches(row, schema);
  print_result("NOT 不匹配 (NOT id=1)", result == false);

  // 2.16 IN 匹配
  auto in_true =
      ConditionExpr::make_in_expr("id", {Value(1), Value(2), Value(3)});
  result = in_true.matches(row, schema);
  print_result("IN 匹配 (id IN (1,2,3))", result == true);

  // 2.17 IN 不匹配
  auto in_false =
      ConditionExpr::make_in_expr("id", {Value(4), Value(5), Value(6)});
  result = in_false.matches(row, schema);
  print_result("IN 不匹配 (id IN (4,5,6))", result == false);

  // 2.18 IN 空列表（边界情况）
  auto in_empty = ConditionExpr::make_in_expr("id", {});
  result = in_empty.matches(row, schema);
  print_result("IN 空列表 (应返回 false)", result == false);
}

// ============================================================
// 测试 3: only_primary_key
// ============================================================
void test_only_primary_key() {
  print_separator("测试 only_primary_key");

  auto schema = create_test_schema();

  // 3.1 主键 EQ
  auto pk_eq = ConditionExpr::make_compare_expr("id", CompareOp::EQ, Value(1));
  bool result = pk_eq.only_primary_key(schema);
  print_result("主键 EQ (id=1)", result == true);

  // 3.2 非主键 EQ
  auto non_pk_eq =
      ConditionExpr::make_compare_expr("name", CompareOp::EQ, Value("Alice"));
  result = non_pk_eq.only_primary_key(schema);
  print_result("非主键 EQ (name='Alice')", result == false);

  // 3.3 主键 IN
  auto pk_in = ConditionExpr::make_in_expr("id", {Value(1), Value(2)});
  result = pk_in.only_primary_key(schema);
  print_result("主键 IN (id IN (1,2))", result == true);

  // 3.4 非主键 IN
  auto non_pk_in =
      ConditionExpr::make_in_expr("name", {Value("Alice"), Value("Bob")});
  result = non_pk_in.only_primary_key(schema);
  print_result("非主键 IN (name IN (...))", result == false);

  // 3.5 AND (主键 AND 主键)
  auto and_pk_pk = ConditionExpr::make_and_expr(
      std::make_unique<ConditionExpr>(
          ConditionExpr::make_compare_expr("id", CompareOp::EQ, Value(1))),
      std::make_unique<ConditionExpr>(
          ConditionExpr::make_compare_expr("id", CompareOp::GT, Value(0))));
  result = and_pk_pk.only_primary_key(schema);
  print_result("AND (主键 AND 主键)", result == true);

  // 3.6 AND (主键 AND 非主键)
  auto and_pk_non = ConditionExpr::make_and_expr(
      std::make_unique<ConditionExpr>(
          ConditionExpr::make_compare_expr("id", CompareOp::EQ, Value(1))),
      std::make_unique<ConditionExpr>(
          ConditionExpr::make_compare_expr("age", CompareOp::GT, Value(18))));
  result = and_pk_non.only_primary_key(schema);
  print_result("AND (主键 AND 非主键)", result == false);
}

// ============================================================
// 测试 4: extract_primary_key_conditions
// ============================================================
void test_extract_pk_conditions() {
  print_separator("测试 extract_primary_key_conditions");

  auto schema = create_test_schema();
  std::vector<std::pair<CompareOp, Value>> pk_conds;
  std::unique_ptr<ConditionExpr> remaining;

  // 4.1 主键 EQ
  auto pk_eq = ConditionExpr::make_compare_expr("id", CompareOp::EQ, Value(1));
  bool has_pk =
      pk_eq.extract_primary_key_conditions(schema, pk_conds, remaining);
  print_result("主键 EQ 提取 (id=1)", has_pk == true && pk_conds.size() == 1);
  if (has_pk) {
    std::cout << "  提取到: " << compare_op_to_string(pk_conds[0].first) << " "
              << pk_conds[0].second.to_string() << std::endl;
  }

  // 4.2 非主键 EQ
  auto non_pk =
      ConditionExpr::make_compare_expr("name", CompareOp::EQ, Value("Alice"));
  has_pk = non_pk.extract_primary_key_conditions(schema, pk_conds, remaining);
  print_result("非主键 EQ 提取 (name='Alice')",
               has_pk == false && remaining != nullptr);

  // 4.3 主键 IN
  auto pk_in =
      ConditionExpr::make_in_expr("id", {Value(1), Value(2), Value(3)});
  has_pk = pk_in.extract_primary_key_conditions(schema, pk_conds, remaining);
  print_result("主键 IN 提取 (id IN (1,2,3))",
               has_pk == true && pk_conds.size() == 3);
  if (has_pk) {
    std::cout << "  提取到 " << pk_conds.size() << " 个条件:" << std::endl;
    for (const auto& [op, val] : pk_conds) {
      std::cout << "    " << compare_op_to_string(op) << " " << val.to_string()
                << std::endl;
    }
  }

  // 4.4 AND (主键 AND 非主键)
  auto mixed = ConditionExpr::make_and_expr(
      std::make_unique<ConditionExpr>(
          ConditionExpr::make_compare_expr("id", CompareOp::EQ, Value(1))),
      std::make_unique<ConditionExpr>(
          ConditionExpr::make_compare_expr("age", CompareOp::GT, Value(18))));
  pk_conds.clear();
  remaining.reset();
  has_pk = mixed.extract_primary_key_conditions(schema, pk_conds, remaining);
  print_result("AND (主键 EQ AND 非主键)",
               has_pk == true && pk_conds.size() == 1 && remaining != nullptr);
  if (remaining) {
    std::cout << "  剩余条件: " << remaining->to_string() << std::endl;
  }

  // 4.5 非主键 OR (应该无法提取)
  auto or_expr = ConditionExpr::make_or_expr(
      std::make_unique<ConditionExpr>(
          ConditionExpr::make_compare_expr("age", CompareOp::EQ, Value(25))),
      std::make_unique<ConditionExpr>(
          ConditionExpr::make_compare_expr("age", CompareOp::EQ, Value(30))));
  pk_conds.clear();
  remaining.reset();
  has_pk = or_expr.extract_primary_key_conditions(schema, pk_conds, remaining);
  print_result("非主键 OR (age=25 OR age=30) 无法提取",
               has_pk == false && remaining != nullptr);
}

// ============================================================
// 主函数
// ============================================================
int main() {
  std::cout << "╔══════════════════════════════════════════╗" << std::endl;
  std::cout << "║     Condition 表达式测试套件            ║" << std::endl;
  std::cout << "╚══════════════════════════════════════════╝" << std::endl;

  test_construction();
  test_matches();
  test_only_primary_key();
  test_extract_pk_conditions();

  std::cout << "\n✅ 所有测试完成!" << std::endl;
  return 0;
}
