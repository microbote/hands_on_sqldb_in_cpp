// test_key_range.cpp
#include "test_framework.h"
#include "sql_types/key_range.h"
#include "sql_types/value.h"

using namespace sql;

TEST(KeyRange, BasicCreation) {
    KeyRange range = KeyRange::range(Value(1), Value(10));
    
    CHECK(!range.is_point());
    CHECK(!range.is_all());
    CHECK_EQ(range.low_value().as_int(), 1);
    CHECK_EQ(range.high_value().as_int(), 10);
}

TEST(KeyRange, PointRange) {
    KeyRange point = KeyRange::point(Value(5));
    CHECK(point.is_point());
    CHECK_EQ(point.low_value().as_int(), 5);
    CHECK_EQ(point.high_value().as_int(), 6);
}

TEST(KeyRange, FullRange) {
    // 全范围扫描
    KeyRange full = KeyRange::all();
    CHECK(full.is_all());
}

TEST(KeyRange, Contains) {
    KeyRange range = KeyRange::range(Value(1), Value(10));
    
    CHECK(range.contains(Value(5)));
    CHECK(range.contains(Value(1)));   // 边界包含
    CHECK(range.contains(Value(10)));  // 边界包含
    CHECK(!range.contains(Value(0)));
    CHECK(!range.contains(Value(11)));
}

TEST(KeyRange, Overlap) {
    KeyRange r1=KeyRange::range(Value(1), Value(5));
    KeyRange r2=KeyRange::range(Value(3), Value(8));
    KeyRange r3=KeyRange::range(Value(10), Value(20));
    
    CHECK(r1.overlaps(r2));
    CHECK(r2.overlaps(r1));
    CHECK(!r1.overlaps(r3));
    CHECK(!r2.overlaps(r3));
    
    // 边界接触
    KeyRange r4 = KeyRange::range(Value(5), Value(15));
    CHECK(r1.overlaps(r4));  // 5 == 5 边界？
}

TEST(KeyRange, Union) {
    KeyRange r1 = KeyRange::range(Value(1), Value(5));
    KeyRange r2 = KeyRange::range(Value(3), Value(8));
    
    auto merged = r1.unite(r2);
    CHECK(!merged.empty());
    CHECK_EQ(merged[0].low_value().as_int(), 1);
    CHECK_EQ(merged[0].high_value().as_int(), 8);
}

TEST(KeyRange, Intersection) {
    KeyRange r1 = KeyRange::range(Value(1), Value(10));
    KeyRange r2= KeyRange::range(Value(3), Value(5));
    
    auto common = r1.intersect(r2);
    CHECK(common.is_nonempty());
    CHECK_EQ(common.low_value().as_int(), 3);
    CHECK_EQ(common.high_value().as_int(), 5);
    
    // 无交集
    KeyRange r3= KeyRange::range(Value(20), Value(30));
    auto empty = r1.intersect(r3);
    CHECK(!empty.is_nonempty());
}

TEST(KeyRange, Difference) {
    KeyRange r1= KeyRange::range(Value(1), Value(10));
    KeyRange r2= KeyRange::range(Value(3), Value(5));
    
    auto remaining = r1.subtract(r2);
    CHECK(remaining.size()>0);
    // 结果是两个区间？(1,2) 和 (6,10)
}

TEST(KeyRange, Comparison) {
    KeyRange r1= KeyRange::range(Value(1), Value(10));
    KeyRange r2= KeyRange::range(Value(1), Value(10));
    KeyRange r3= KeyRange::range(Value(1), Value(20));
    
    CHECK(r1 == r2);
    CHECK(r1 != r3);
}

TEST(KeyRange, ToString) {
    KeyRange range = KeyRange::range(Value(1), Value(10));
    std::string s = range.to_string();
    CHECK(!s.empty());
    CHECK(s.find("1") != std::string::npos);
    CHECK(s.find("10") != std::string::npos);
}

TEST(KeyRange, StrKeyForInt) {
  auto range = KeyRange::all(DataType::INT);
  auto skr = range.to_str_key_range();
  CHECK(skr.low < skr.high);
  auto bounded = KeyRange::range(Value(1), Value(10000)).to_str_key_range();
  CHECK(skr.low < bounded.low);
  CHECK(skr.high > bounded.high);
  CHECK(bounded.low < bounded.high);
}

TEST(KeyRange, StrKeyForStr) {
  auto range = KeyRange::all(DataType::VARCHAR);
  auto skr = range.to_str_key_range();
  CHECK(skr.low < skr.high);
  auto bounded = KeyRange::range(Value("aaaa"), Value("zzzz")).to_str_key_range();
  CHECK(skr.low < bounded.low);
  CHECK(skr.high > bounded.high);
  CHECK(bounded.low < bounded.high);
}