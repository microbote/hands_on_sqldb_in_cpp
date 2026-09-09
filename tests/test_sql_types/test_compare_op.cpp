// test_compare_op.cpp
#include "test_framework.h"
#include "sql_types/compare_op.h"

using namespace sql;

TEST(CompareOp, StringConversion) {
    CHECK_EQ(compare_op_to_string(CompareOp::EQ), "=");
    CHECK_EQ(compare_op_to_string(CompareOp::NE), "!=");
    CHECK_EQ(compare_op_to_string(CompareOp::GT), ">");
    CHECK_EQ(compare_op_to_string(CompareOp::GE), ">=");
    CHECK_EQ(compare_op_to_string(CompareOp::LT), "<");
    CHECK_EQ(compare_op_to_string(CompareOp::LE), "<=");
    CHECK_EQ(compare_op_to_string(CompareOp::LIKE), "LIKE");
    CHECK_EQ(compare_op_to_string(CompareOp::NOT_LIKE), "NOT LIKE");
    CHECK_EQ(compare_op_to_string(CompareOp::IS_NULL), "IS NULL");
    CHECK_EQ(compare_op_to_string(CompareOp::IS_NOT_NULL), "IS NOT NULL");
}

TEST(CompareOp, StringParse) {
    CHECK(string_to_compare_op("=") == CompareOp::EQ);
    CHECK(string_to_compare_op("!=") == CompareOp::NE);
    CHECK(string_to_compare_op("<>") == CompareOp::NE);
    CHECK(string_to_compare_op(">") == CompareOp::GT);
    CHECK(string_to_compare_op(">=") == CompareOp::GE);
    CHECK(string_to_compare_op("<") == CompareOp::LT);
    CHECK(string_to_compare_op("<=") == CompareOp::LE);
    
    // 大小写不敏感
    CHECK(string_to_compare_op("like") == CompareOp::LIKE);
    CHECK(string_to_compare_op("NOT LIKE") == CompareOp::NOT_LIKE);
    CHECK(string_to_compare_op("is null") == CompareOp::IS_NULL);
    CHECK(string_to_compare_op("IS NOT NULL") == CompareOp::IS_NOT_NULL);
    
    // 无效输入
    CHECK(string_to_compare_op("INVALID") == CompareOp::UNKNOWN);
    CHECK(string_to_compare_op("") == CompareOp::UNKNOWN);
}

TEST(CompareOp, Properties) {
    // 范围操作
    CHECK(is_range_op(CompareOp::GT));
    CHECK(is_range_op(CompareOp::GE));
    CHECK(is_range_op(CompareOp::LT));
    CHECK(is_range_op(CompareOp::LE));
    CHECK(!is_range_op(CompareOp::EQ));
    CHECK(!is_range_op(CompareOp::LIKE));
    
    // 等值操作
    CHECK(is_equality_op(CompareOp::EQ));
    CHECK(is_equality_op(CompareOp::NE));
    CHECK(!is_equality_op(CompareOp::GT));
    
    // NULL 判断
    CHECK(is_null_op(CompareOp::IS_NULL));
    CHECK(is_null_op(CompareOp::IS_NOT_NULL));
    CHECK(!is_null_op(CompareOp::EQ));
    
    // LIKE 操作
    CHECK(is_like_op(CompareOp::LIKE));
    CHECK(is_like_op(CompareOp::NOT_LIKE));
    CHECK(!is_like_op(CompareOp::EQ));
}

TEST(CompareOp, Indexable) {
    CHECK(is_indexable(CompareOp::EQ));
    CHECK(is_indexable(CompareOp::GT));
    CHECK(is_indexable(CompareOp::GE));
    CHECK(is_indexable(CompareOp::LT));
    CHECK(is_indexable(CompareOp::LE));
    CHECK(!is_indexable(CompareOp::NE));
    CHECK(!is_indexable(CompareOp::LIKE));
    CHECK(!is_indexable(CompareOp::IS_NULL));
}

TEST(CompareOp, RequireFullScan) {
    CHECK(require_full_scan(CompareOp::LIKE));
    CHECK(require_full_scan(CompareOp::NOT_LIKE));
    CHECK(require_full_scan(CompareOp::IS_NULL));
    CHECK(require_full_scan(CompareOp::IS_NOT_NULL));
    CHECK(!require_full_scan(CompareOp::EQ));
    CHECK(!require_full_scan(CompareOp::GT));
}

TEST(CompareOp, FlipOp) {
    CHECK(flip_compare_op(CompareOp::EQ) == CompareOp::NE);
    CHECK(flip_compare_op(CompareOp::NE) == CompareOp::EQ);
    CHECK(flip_compare_op(CompareOp::GT) == CompareOp::LE);
    CHECK(flip_compare_op(CompareOp::GE) == CompareOp::LT);
    CHECK(flip_compare_op(CompareOp::LT) == CompareOp::GE);
    CHECK(flip_compare_op(CompareOp::LE) == CompareOp::GT);
    CHECK(flip_compare_op(CompareOp::LIKE) == CompareOp::NOT_LIKE);
    CHECK(flip_compare_op(CompareOp::NOT_LIKE) == CompareOp::LIKE);
    CHECK(flip_compare_op(CompareOp::IS_NULL) == CompareOp::IS_NOT_NULL);
    CHECK(flip_compare_op(CompareOp::IS_NOT_NULL) == CompareOp::IS_NULL);
    CHECK(flip_compare_op(CompareOp::UNKNOWN) == CompareOp::UNKNOWN);
}