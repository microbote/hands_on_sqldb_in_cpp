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
    CHECK(!range.contains(Value(10)));  // 边界不包含
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
    CHECK(!r1.overlaps(r4));  // 5 == 5 边界？
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

// ============================================================
// 边界情形（之前的实现要么漏分支，要么直接不支持）
// ============================================================

TEST(KeyRange, SubtractUnboundedHigh) {
  // [1, +∞) - [1, 5) 应该是 [5, +∞)，旧实现返回 ∅
  auto diff = KeyRange::from(Value(1)).subtract(KeyRange::range(Value(1), Value(5)));
  CHECK_EQ(diff.size(), 1);
  CHECK(!diff[0].is_empty());
  CHECK(diff[0].contains(Value(5)));
  CHECK(diff[0].contains(Value(100)));
  CHECK(!diff[0].contains(Value(4)));
}

TEST(KeyRange, SubtractUnboundedLow) {
  // (-∞, 5) - (-∞, 3) 应该是 [3, 5)
  auto diff = KeyRange::to(Value(5)).subtract(KeyRange::to(Value(3)));
  CHECK_EQ(diff.size(), 1);
  CHECK(diff[0].contains(Value(3)));
  CHECK(diff[0].contains(Value(4)));
  CHECK(!diff[0].contains(Value(2)));
  CHECK(!diff[0].contains(Value(5)));
}

TEST(KeyRange, SubtractProducesTwoParts) {
  auto diff = KeyRange::range(Value(1), Value(10)).subtract(
      KeyRange::range(Value(3), Value(5)));
  CHECK_EQ(diff.size(), 2);
  CHECK(diff[0].contains(Value(1)));
  CHECK(diff[0].contains(Value(2)));
  CHECK(!diff[0].contains(Value(3)));
  CHECK(diff[1].contains(Value(5)));
  CHECK(diff[1].contains(Value(9)));
  CHECK(!diff[1].contains(Value(10)));
}

TEST(KeyRange, ComplementKeepsBoundaryExact) {
  // [3,5] 的补集 = (-∞,3) ∪ (5,+∞)
  auto comp = KeyRange::closed(Value(3), Value(5)).complement();
  CHECK_EQ(comp.size(), 2);
  CHECK(!comp[0].contains(Value(3)));
  CHECK(comp[0].contains(Value(2)));
  CHECK(!comp[1].contains(Value(5)));
  CHECK(comp[1].contains(Value(6)));

  // [3,5) 的补集 = (-∞,3) ∪ [5,+∞)
  auto comp2 = KeyRange::range(Value(3), Value(5)).complement();
  CHECK_EQ(comp2.size(), 2);
  CHECK(!comp2[0].contains(Value(3)));
  CHECK(comp2[1].contains(Value(5)));
}

TEST(KeyRange, ClosureAndAdjacency) {
  KeyRange a = KeyRange::range(Value(1), Value(5));
  KeyRange b = KeyRange::range(Value(3), Value(8));
  CHECK(a.covers(a));
  CHECK(!a.covers(b));
  CHECK(!b.covers(a));
  KeyRange wide = KeyRange::range(Value(1), Value(8));
  CHECK(wide.covers(a));
  CHECK(wide.covers(b));

  KeyRange next = KeyRange::range(Value(5), Value(9));
  CHECK(!a.overlaps(next));       // a 的 5 是开的
  CHECK(a.is_adjacent(next));     // 但两者相邻，可以合并
  auto merged = a.unite(next);
  CHECK_EQ(merged.size(), 1);
  CHECK(merged[0].contains(Value(1)));
  CHECK(merged[0].contains(Value(8)));

  KeyRange overlapping = KeyRange::range(Value(4), Value(9));
  CHECK(a.overlaps(overlapping));
}

TEST(KeyRange, StringPointAndClosedRange) {
  // 字符串单点：旧实现直接返回空集
  KeyRange p = KeyRange::point(Value(std::string("a")));
  CHECK(!p.is_empty());
  CHECK(p.is_point());
  CHECK(p.contains(Value(std::string("a"))));
  CHECK(!p.contains(Value(std::string("ab"))));
  CHECK(!p.contains(Value(std::string("a\0", 2))));
  CHECK(!p.contains(Value(std::string(""))));

  // 字符串闭区间 [a, c]
  KeyRange c = KeyRange::closed(Value(std::string("a")), Value(std::string("c")));
  CHECK(c.contains(Value(std::string("a"))));
  CHECK(c.contains(Value(std::string("b"))));
  CHECK(c.contains(Value(std::string("c"))));
  CHECK(!c.contains(Value(std::string("d"))));
}

TEST(KeyRange, EncodedRangeMatchesValueSemantics) {
  // 关键不变量：contains() 的答案必须和"在编码后 key 空间里扫描"一致，
  // 否则优化器给出的范围会扫丢数据（字符串长度前缀编码就踩过这个坑）。
  const std::string samples[] = {"", "a", "aa", "aaaa", "ab", "b", "zz"};
  KeyRange r = KeyRange::range(Value(std::string("a")), Value(std::string("b")));
  auto skr = r.to_str_key_range();

  for (const auto& s : samples) {
    const Key k = Value(s).to_key();
    const bool in_key_space = (skr.low <= k) && (k < skr.high);
    CHECK_EQ(r.contains(Value(s)), in_key_space);
  }
}

TEST(KeyRange, IntegerRangeMatchesEncodedRange) {
  KeyRange r = KeyRange::range(Value(10), Value(20));
  auto skr = r.to_str_key_range();
  for (int v = 0; v <= 25; ++v) {
    const Key k = Value(v).to_key();
    const bool in_key_space = (skr.low <= k) && (k < skr.high);
    CHECK_EQ(r.contains(Value(v)), in_key_space);
  }
}

TEST(KeyRange, TypeIsPropagatedThroughSetOperations) {
  KeyRange a = KeyRange::range(Value(1), Value(10));
  KeyRange b = KeyRange::range(Value(3), Value(8));
  CHECK(a.intersect(b).type() == DataType::INT);
  CHECK(a.unite(b)[0].type() == DataType::INT);
  CHECK(a.subtract(b)[0].type() == DataType::INT);

  KeyRange unknown = KeyRange::all();
  CHECK(unknown.intersect(a).type() == DataType::INT);
  CHECK(a.intersect(unknown).type() == DataType::INT);
}

TEST(KeyRange, UnboundedStringRangeCoversEveryString) {
  auto all_str = KeyRange::all(DataType::VARCHAR).to_str_key_range();
  const std::string samples[] = {"", "a", "aaaa", "b", std::string(200, 'z'),
                                 std::string("\xff\xff", 2)};
  for (const auto& s : samples) {
    const Key k = Value(s).to_key();
    CHECK(all_str.low <= k);
    CHECK(k < all_str.high);
  }
  // 整数 key 不在字符串族的范围内
  const Key int_key = Value(1).to_key();
  CHECK(int_key < all_str.low);
}
