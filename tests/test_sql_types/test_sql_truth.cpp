// tests/test_sql_types/test_sql_truth.cpp
//
// SQL 三值逻辑：比较语义、真值表、类型提升、WHERE 过滤。
#include "test_framework.h"
#include "sql_types/sql_truth.h"

using namespace sql;

TEST(Truth, NotIsThreeValued) {
    CHECK(truth_not(Truth::TRUE) == Truth::FALSE);
    CHECK(truth_not(Truth::FALSE) == Truth::TRUE);
    CHECK(truth_not(Truth::UNKNOWN) == Truth::UNKNOWN);
}

TEST(Truth, AndTruthTable) {
    using T = Truth;
    CHECK(truth_and(T::TRUE, T::TRUE) == T::TRUE);
    CHECK(truth_and(T::TRUE, T::FALSE) == T::FALSE);
    CHECK(truth_and(T::TRUE, T::UNKNOWN) == T::UNKNOWN);
    CHECK(truth_and(T::FALSE, T::FALSE) == T::FALSE);
    CHECK(truth_and(T::FALSE, T::UNKNOWN) == T::FALSE);   // FALSE 一票否决
    CHECK(truth_and(T::UNKNOWN, T::UNKNOWN) == T::UNKNOWN);
}

TEST(Truth, OrTruthTable) {
    using T = Truth;
    CHECK(truth_or(T::TRUE, T::TRUE) == T::TRUE);
    CHECK(truth_or(T::TRUE, T::FALSE) == T::TRUE);
    CHECK(truth_or(T::TRUE, T::UNKNOWN) == T::TRUE);      // TRUE 一票通过
    CHECK(truth_or(T::FALSE, T::FALSE) == T::FALSE);
    CHECK(truth_or(T::FALSE, T::UNKNOWN) == T::UNKNOWN);
    CHECK(truth_or(T::UNKNOWN, T::UNKNOWN) == T::UNKNOWN);
}

TEST(Truth, WhereOnlyKeepsTrue) {
    CHECK(where_keeps(Truth::TRUE));
    CHECK(!where_keeps(Truth::FALSE));
    CHECK(!where_keeps(Truth::UNKNOWN));   // UNKNOWN 的行会被过滤
}

TEST(Truth, CompareWithNullIsUnknown) {
    const Value null_value;
    const Value five(5);

    CHECK(sql_compare_op(CompareOp::EQ, null_value, five) == Truth::UNKNOWN);
    CHECK(sql_compare_op(CompareOp::LT, null_value, five) == Truth::UNKNOWN);
    CHECK(sql_compare_op(CompareOp::NE, null_value, five) == Truth::UNKNOWN);
    // NULL = NULL 也是 UNKNOWN（不是 TRUE）
    CHECK(sql_compare_op(CompareOp::EQ, null_value, Value()) == Truth::UNKNOWN);
    CHECK(sql_compare_op(CompareOp::NE, null_value, Value()) == Truth::UNKNOWN);
    CHECK(sql_equal(null_value, null_value) == Truth::UNKNOWN);
    CHECK(!sql_order(null_value, five).has_value());
}

TEST(Truth, IsNullPredicatesAreTwoValued) {
    const Value null_value;
    const Value five(5);

    CHECK(sql_compare_op(CompareOp::IS_NULL, null_value, Value()) ==
          Truth::TRUE);
    CHECK(sql_compare_op(CompareOp::IS_NOT_NULL, null_value, Value()) ==
          Truth::FALSE);
    CHECK(sql_compare_op(CompareOp::IS_NULL, five, Value()) == Truth::FALSE);
    CHECK(sql_compare_op(CompareOp::IS_NOT_NULL, five, Value()) == Truth::TRUE);
}

TEST(Truth, IntegerFamilyComparesAcrossWidths) {
    // TINYINT/SMALLINT/INT/BIGINT 提升到公共类型后比较
    CHECK(sql_compare_op(CompareOp::EQ, Value::tinyint(5), Value::bigint(5)) ==
          Truth::TRUE);
    CHECK(sql_compare_op(CompareOp::LT, Value::tinyint(5), Value::smallint(6)) ==
          Truth::TRUE);
    CHECK(sql_compare_op(CompareOp::GE, Value::bigint(300), Value::tinyint(5)) ==
          Truth::TRUE);

    // 布尔按 0/1 参与数值比较（MySQL: BOOLEAN = TINYINT(1)）
    CHECK(sql_compare_op(CompareOp::EQ, Value(true), Value(1)) == Truth::TRUE);
    CHECK(sql_compare_op(CompareOp::EQ, Value(false), Value(0)) == Truth::TRUE);
}

TEST(Truth, StringComparisonIsLexicographic) {
    CHECK(sql_compare_op(CompareOp::LT, Value(std::string("apple")),
                         Value(std::string("banana"))) == Truth::TRUE);
    CHECK(sql_compare_op(CompareOp::EQ, Value::text("x"), Value("x")) ==
          Truth::TRUE);
}

TEST(Truth, CrossFamilyPromotionNumberVsString) {
    // 数字与字符串：字符串按整数解析后再比较
    CHECK(sql_compare_op(CompareOp::LT, Value(5), Value(std::string("7"))) ==
          Truth::TRUE);
    CHECK(sql_compare_op(CompareOp::GT, Value(10), Value(std::string("9"))) ==
          Truth::TRUE);
    CHECK(sql_compare_op(CompareOp::EQ, Value(7), Value(std::string("7"))) ==
          Truth::TRUE);
    // 交换顺序结果要一致
    CHECK(sql_compare_op(CompareOp::GT, Value(std::string("7")), Value(5)) ==
          Truth::TRUE);

    // 非数字字符串 -> UNKNOWN（比 MySQL 的"按 0 处理"更保守）
    CHECK(sql_compare_op(CompareOp::EQ, Value(0), Value(std::string("abc"))) ==
          Truth::UNKNOWN);
}

TEST(Truth, TemporalPromotionAndParsing) {
    // DATE 与 DATETIME 提升到秒
    CHECK(sql_compare_op(CompareOp::EQ, Value::date(1), Value::datetime(86400)) ==
          Truth::TRUE);
    CHECK(sql_compare_op(CompareOp::LT, Value::date(1), Value::datetime(90000)) ==
          Truth::TRUE);
    // 时间与字符串：字符串按时间类型解析
    CHECK(sql_compare_op(CompareOp::EQ, Value::date(0),
                         Value(std::string("1970-01-01"))) == Truth::TRUE);
    CHECK(sql_compare_op(CompareOp::GT, Value::datetime(0),
                         Value(std::string("1969-12-31 23:59:59"))) ==
          Truth::TRUE);
    // 无法解析的字符串 -> UNKNOWN
    CHECK(sql_compare_op(CompareOp::EQ, Value::date(0),
                         Value(std::string("not-a-date"))) == Truth::UNKNOWN);
    // TIME 与 DATE 不做隐式换算
    CHECK(sql_compare_op(CompareOp::EQ, Value::time(0), Value::date(0)) ==
          Truth::UNKNOWN);
}

TEST(Truth, LikeMatching) {
    CHECK(like_match("hello", "hello"));
    CHECK(like_match("hello", "hel%"));
    CHECK(like_match("hello", "%llo"));
    CHECK(like_match("hello", "h%o"));
    CHECK(like_match("hello", "h_llo"));
    CHECK(like_match("hello", "%"));
    CHECK(like_match("", "%"));
    CHECK(!like_match("hello", "h_lo"));
    CHECK(!like_match("hello", "%x%"));
    // 转义
    CHECK(like_match("100%", "100\\%"));
    CHECK(!like_match("1000", "100\\%"));
    // 百分号在中间
    CHECK(like_match("aXbYc", "a%b%c"));
    CHECK(!like_match("aXbY", "a%b%c"));

    CHECK(sql_compare_op(CompareOp::LIKE, Value(std::string("hello")),
                         Value(std::string("h%"))) == Truth::TRUE);
    CHECK(sql_compare_op(CompareOp::NOT_LIKE, Value(std::string("hello")),
                         Value(std::string("x%"))) == Truth::TRUE);
    // NULL 参与 LIKE -> UNKNOWN
    CHECK(sql_compare_op(CompareOp::LIKE, Value(), Value(std::string("h%"))) ==
          Truth::UNKNOWN);
}

TEST(Truth, NullSafeEqualityForGrouping) {
    // DISTINCT / GROUP BY / JOIN 键：NULL 与 NULL 视为一组
    CHECK(sql_equal_null_safe(Value(), Value()) == Truth::TRUE);
    CHECK(sql_equal_null_safe(Value(), Value(1)) == Truth::FALSE);
    CHECK(sql_equal_null_safe(Value(1), Value(1)) == Truth::TRUE);
}

// ============================================================
// 条件树求值（WHERE）
// ============================================================

TEST(Truth, ConditionEvaluationFiltersUnknownRows) {
    // age > 18，age 为 NULL -> UNKNOWN -> 被 WHERE 过滤掉
    auto cond = make_compare("age", CompareOp::GT, Value(18));

    const Value age_null;
    Value stored_null = age_null;
    const auto null_result = evaluate_condition(
        *cond, [&stored_null](const Identifier& col) -> const Value* {
            return col == "age" ? &stored_null : nullptr;
        });
    CHECK(null_result == Truth::UNKNOWN);
    CHECK(!where_keeps(null_result));

    Value age_20(20);
    const auto match_result = evaluate_condition(
        *cond, [&age_20](const Identifier& col) -> const Value* {
            return col == "age" ? &age_20 : nullptr;
        });
    CHECK(match_result == Truth::TRUE);
    CHECK(where_keeps(match_result));

    // 列不存在 -> UNKNOWN
    const auto missing = evaluate_condition(
        *cond, [](const Identifier&) -> const Value* { return nullptr; });
    CHECK(missing == Truth::UNKNOWN);
}

TEST(Truth, NotInWithNullIsUnknown) {
    // 20 IN (18, NULL) -> FALSE OR UNKNOWN = UNKNOWN
    Value age_20(20);
    auto in_cond = make_in("age", false, {Value(18), Value()});
    const auto in_result = evaluate_condition(
        *in_cond, [&age_20](const Identifier& col) -> const Value* {
            return col == "age" ? &age_20 : nullptr;
        });
    CHECK(in_result == Truth::UNKNOWN);

    // 20 NOT IN (18, NULL) -> NOT UNKNOWN = UNKNOWN（不会返回 TRUE！）
    auto not_in_cond = make_in("age", true, {Value(18), Value()});
    const auto not_in_result = evaluate_condition(
        *not_in_cond, [&age_20](const Identifier& col) -> const Value* {
            return col == "age" ? &age_20 : nullptr;
        });
    CHECK(not_in_result == Truth::UNKNOWN);

    // 命中时是 TRUE：20 IN (20, NULL)
    auto hit = make_in("age", false, {Value(20), Value()});
    CHECK(evaluate_condition(
              *hit, [&age_20](const Identifier& col) -> const Value* {
                  return col == "age" ? &age_20 : nullptr;
              }) == Truth::TRUE);
}

TEST(Truth, AndOrNotShortCircuitWithUnknown) {
    // (FALSE AND UNKNOWN) = FALSE，(TRUE OR UNKNOWN) = TRUE
    Value flag_false(false);
    Value flag_true(true);

    // flag = true 在 flag=false 时不成立 -> FALSE，与 UNKNOWN 相与仍是 FALSE
    auto and_cond = make_and(make_compare("flag", CompareOp::EQ, Value(true)),
                             make_compare("missing", CompareOp::EQ, Value(1)));
    CHECK(evaluate_condition(
              *and_cond, [&flag_false](const Identifier& col) -> const Value* {
                  return col == "flag" ? &flag_false : nullptr;
              }) == Truth::FALSE);

    auto or_cond = make_or(make_compare("flag", CompareOp::EQ, Value(true)),
                           make_compare("missing", CompareOp::EQ, Value(1)));
    CHECK(evaluate_condition(
              *or_cond, [&flag_true](const Identifier& col) -> const Value* {
                  return col == "flag" ? &flag_true : nullptr;
              }) == Truth::TRUE);
}

TEST(Truth, IsNullConditionKeepsNullRows) {
    // WHERE name IS NULL 应该保留 NULL 行（TRUE）
    auto cond = make_compare("name", CompareOp::IS_NULL, Value());
    Value null_name;
    CHECK(where_keeps(evaluate_condition(
        *cond, [&null_name](const Identifier& col) -> const Value* {
            return col == "name" ? &null_name : nullptr;
        })));

    Value name("bob");
    CHECK(!where_keeps(evaluate_condition(
        *cond, [&name](const Identifier& col) -> const Value* {
            return col == "name" ? &name : nullptr;
        })));
}
