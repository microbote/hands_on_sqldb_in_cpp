// test_key_range.cpp
#include "sql_types/key_range.h"
#include "sql_types/value.h"
#include "test_framework.h"

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
  // 单点统一用 [v, v]（不再用 [v, v+1)：避免类型最大值处的 +1 溢出）
  CHECK_EQ(point.high_value().as_int(), 5);
  CHECK(point.high_inclusive());
  CHECK_EQ(point.size().value_or(0), 1);

  // 类型最大值处也能安全取单点
  KeyRange max_point = KeyRange::point(Value::bigint(INT64_MAX));
  CHECK(max_point.is_point());
  CHECK(max_point.contains(Value::bigint(INT64_MAX)));
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
  CHECK(!range.contains(Value(10))); // 边界不包含
  CHECK(!range.contains(Value(0)));
  CHECK(!range.contains(Value(11)));
}

TEST(KeyRange, Overlap) {
  KeyRange r1 = KeyRange::range(Value(1), Value(5));
  KeyRange r2 = KeyRange::range(Value(3), Value(8));
  KeyRange r3 = KeyRange::range(Value(10), Value(20));

  CHECK(r1.overlaps(r2));
  CHECK(r2.overlaps(r1));
  CHECK(!r1.overlaps(r3));
  CHECK(!r2.overlaps(r3));

  // 边界接触
  KeyRange r4 = KeyRange::range(Value(5), Value(15));
  CHECK(!r1.overlaps(r4)); // 5 == 5 边界？
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
  KeyRange r2 = KeyRange::range(Value(3), Value(5));

  auto common = r1.intersect(r2);
  CHECK(common.is_nonempty());
  CHECK_EQ(common.low_value().as_int(), 3);
  CHECK_EQ(common.high_value().as_int(), 5);

  // 无交集
  KeyRange r3 = KeyRange::range(Value(20), Value(30));
  auto empty = r1.intersect(r3);
  CHECK(!empty.is_nonempty());
}

TEST(KeyRange, Difference) {
  KeyRange r1 = KeyRange::range(Value(1), Value(10));
  KeyRange r2 = KeyRange::range(Value(3), Value(5));

  auto remaining = r1.subtract(r2);
  CHECK(remaining.size() > 0);
  // 结果是两个区间？(1,2) 和 (6,10)
}

TEST(KeyRange, Comparison) {
  KeyRange r1 = KeyRange::range(Value(1), Value(10));
  KeyRange r2 = KeyRange::range(Value(1), Value(10));
  KeyRange r3 = KeyRange::range(Value(1), Value(20));

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
  CHECK(skr.start < skr.end);
  auto bounded = KeyRange::range(Value(1), Value(10000)).to_str_key_range();
  CHECK(skr.start < bounded.start);
  CHECK(skr.end > bounded.end);
  CHECK(bounded.start < bounded.end);
}

TEST(KeyRange, StrKeyForStr) {
  auto range = KeyRange::all(DataType::VARCHAR);
  auto skr = range.to_str_key_range();
  CHECK(skr.start < skr.end);
  auto bounded =
      KeyRange::range(Value("aaaa"), Value("zzzz")).to_str_key_range();
  CHECK(skr.start < bounded.start);
  CHECK(skr.end > bounded.end);
  CHECK(bounded.start < bounded.end);
}

// ============================================================
// 边界情形（之前的实现要么漏分支，要么直接不支持）
// ============================================================

TEST(KeyRange, SubtractUnboundedHigh) {
  // [1, +∞) - [1, 5) 应该是 [5, +∞)，旧实现返回 ∅
  auto diff =
      KeyRange::from(Value(1)).subtract(KeyRange::range(Value(1), Value(5)));
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
  auto diff = KeyRange::range(Value(1), Value(10))
                  .subtract(KeyRange::range(Value(3), Value(5)));
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
  CHECK(!a.overlaps(next));   // a 的 5 是开的
  CHECK(a.is_adjacent(next)); // 但两者相邻，可以合并
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
  KeyRange c =
      KeyRange::closed(Value(std::string("a")), Value(std::string("c")));
  CHECK(c.contains(Value(std::string("a"))));
  CHECK(c.contains(Value(std::string("b"))));
  CHECK(c.contains(Value(std::string("c"))));
  CHECK(!c.contains(Value(std::string("d"))));
}

TEST(KeyRange, EncodedRangeMatchesValueSemantics) {
  // 关键不变量：contains() 的答案必须和"在编码后 key 空间里扫描"一致，
  // 否则优化器给出的范围会扫丢数据（字符串长度前缀编码就踩过这个坑）。
  const std::string samples[] = {"", "a", "aa", "aaaa", "ab", "b", "zz"};
  KeyRange r =
      KeyRange::range(Value(std::string("a")), Value(std::string("b")));
  auto skr = r.to_str_key_range();

  for (const auto &s : samples) {
    const Key k = Value(s).to_key();
    const bool in_key_space = (skr.start <= k) && (k < skr.end);
    CHECK_EQ(r.contains(Value(s)), in_key_space);
  }
}

TEST(KeyRange, IntegerRangeMatchesEncodedRange) {
  KeyRange r = KeyRange::range(Value(10), Value(20));
  auto skr = r.to_str_key_range();
  for (int v = 0; v <= 25; ++v) {
    const Key k = Value(v).to_key();
    const bool in_key_space = (skr.start <= k) && (k < skr.end);
    CHECK_EQ(r.contains(Value(v)), in_key_space);
  }
}

TEST(KeyRange, TypeIsPropagatedThroughSetOperations) {
  KeyRange a = KeyRange::range(Value(1), Value(10));
  KeyRange b = KeyRange::range(Value(3), Value(8));
  // Value 里的整数统一规范化成 BIGINT（存储层只区分 family）
  CHECK(a.intersect(b).type() == DataType::BIGINT);
  CHECK(a.unite(b)[0].type() == DataType::BIGINT);
  CHECK(a.subtract(b)[0].type() == DataType::BIGINT);

  KeyRange unknown = KeyRange::all();
  CHECK(unknown.intersect(a).type() == DataType::BIGINT);
  CHECK(a.intersect(unknown).type() == DataType::BIGINT);
}

TEST(KeyRange, UnboundedStringRangeCoversEveryString) {
  auto all_str = KeyRange::all(DataType::VARCHAR).to_str_key_range();
  const std::string samples[] = {
      "", "a", "aaaa", "b", std::string(200, 'z'), std::string("\xff\xff", 2)};
  for (const auto &s : samples) {
    const Key k = Value(s).to_key();
    CHECK(all_str.start <= k);
    CHECK(k < all_str.end);
  }
  // 整数 key 不在字符串族的范围内
  const Key int_key = Value(1).to_key();
  CHECK(int_key < all_str.start);
}

// ============================================================
// StrKeyRange：压平后的物理半开区间 [start, end)
// ============================================================

TEST(StrKeyRange, UnboundedRangeFlattensToFamilyBounds) {
  auto all = KeyRange::all(DataType::BIGINT).to_str_key_range();
  // -∞ = 族最小值（NULL 的 key）；+∞ = 族上界
  CHECK(all.start == Value::min_key_for_type(DataType::BIGINT));
  CHECK(all.end == Value::upper_key_for_type(DataType::BIGINT));
  CHECK(!all.is_empty());

  // [5, +∞) / (-∞, 5)
  auto from = KeyRange::from(Value(5)).to_str_key_range();
  CHECK(from.start == Value(5).to_key());
  CHECK(from.end == Value::upper_key_for_type(DataType::BIGINT));

  auto to = KeyRange::to(Value(5)).to_str_key_range();
  CHECK(to.start == Value::min_key_for_type(DataType::BIGINT));
  CHECK(to.end == Value(5).to_key());
}

TEST(StrKeyRange, OpenRangeIsAlreadyHalfOpen) {
  auto skr = KeyRange::range(Value(1), Value(10)).to_str_key_range();
  CHECK(skr.start == Value(1).to_key());
  CHECK(skr.end == Value(10).to_key()); // 开区间：不加后缀
  CHECK(!skr.is_empty());
}

TEST(StrKeyRange, ClosedAndOpenBoundsFlattenToOneForm) {
  // [1,10] → end = key(10)+0x00；半开与开区间只差一个后缀
  const Key key10 = Value(10).to_key();
  auto closed = KeyRange::closed(Value(1), Value(10)).to_str_key_range();
  CHECK(closed.start == Value(1).to_key());
  CHECK(closed.end == KeyCodecs::inclusive_upper_bound(key10));
  CHECK(closed.end > key10);
  CHECK(closed.end < Value::upper_key_for_type(DataType::BIGINT));

  // (1,10] → start = key(1)+0x00
  auto parts = KeyRange::closed(Value(1), Value(10))
                   .subtract(KeyRange::closed(Value(1), Value(5)));
  CHECK_EQ(parts.size(), 1);
  if (parts.size() == 1) {
    auto skr = parts[0].to_str_key_range();
    CHECK(skr.start == KeyCodecs::inclusive_upper_bound(Value(5).to_key()));
    CHECK(skr.end == KeyCodecs::inclusive_upper_bound(Value(10).to_key()));
    CHECK(!skr.is_empty());
  }
}

TEST(StrKeyRange, ClosedRangeAtTypeMaxIsExpressible) {
  auto skr =
      KeyRange::closed(Value::bigint(INT64_MAX - 2), Value::bigint(INT64_MAX))
          .to_str_key_range();
  const Key key_max = Value::bigint(INT64_MAX).to_key();
  CHECK(skr.end == KeyCodecs::inclusive_upper_bound(key_max));
  CHECK(skr.end > key_max);                                     // 含最大值
  CHECK(skr.end < Value::upper_key_for_type(DataType::BIGINT)); // 不越族
  CHECK(!skr.is_empty());
}

TEST(StrKeyRange, EmptyRangeFlattensToInvertedOrEqualBounds) {
  auto empty = KeyRange::empty(DataType::BIGINT).to_str_key_range();
  CHECK(empty.is_empty()); // start >= end
  CHECK(!(empty.start < empty.end));

  // 手工构造的半开空集
  StrKeyRange half_open_empty{Value(1).to_key(), Value(1).to_key()};
  CHECK(half_open_empty.is_empty());

  // 倒置
  StrKeyRange inverted{Value(10).to_key(), Value(1).to_key()};
  CHECK(inverted.is_empty());
}

// ============================================================
// NULL 语义：KeyRange 提供"含/不含/只有 NULL"的能力
// ============================================================

TEST(KeyRangeNull, AllIncludesNullByDefault) {
  auto range = KeyRange::all(DataType::BIGINT);
  CHECK(range.includes_null());
  CHECK(range.null_scope() == NullScope::INCLUDE);
  CHECK(range.contains(Value()));

  auto skr = range.to_str_key_range();
  CHECK(skr.start == Value::null_key_for_type(DataType::BIGINT));
  CHECK(skr.end == Value::upper_key_for_type(DataType::BIGINT));
}

TEST(KeyRangeNull, NonNullExcludesNull) {
  auto range = KeyRange::non_null(DataType::BIGINT);
  CHECK(!range.includes_null());
  CHECK(range.null_scope() == NullScope::EXCLUDE);
  CHECK(!range.contains(Value()));

  // 物理上：start = NULL 的 key + 0x00（等价于 [tag][0x01]：所有非 NULL 值 key
  // 的前缀）
  auto skr = range.to_str_key_range();
  const Key null_key = Value::null_key_for_type(DataType::BIGINT);
  CHECK(skr.start == KeyCodecs::inclusive_upper_bound(null_key));
  CHECK(skr.start > null_key);
  CHECK(skr.start <= Value::first_value_key_for_type(DataType::BIGINT));
  CHECK(skr.end == Value::upper_key_for_type(DataType::BIGINT));

  // 不包含 NULL，但覆盖所有值
  CHECK(range.contains(Value(0)));
  CHECK(range.contains(Value(INT64_MIN)));
  CHECK(range.contains(Value(INT64_MAX)));
}

TEST(KeyRangeNull, NullOnlyIsAPoint) {
  auto range = KeyRange::null_only(DataType::BIGINT);
  CHECK(range.null_scope() == NullScope::ONLY);
  CHECK(range.includes_null());
  CHECK(range.is_point());
  CHECK(range.contains(Value()));
  CHECK(!range.contains(Value(0)));

  auto skr = range.to_str_key_range();
  const Key null_key = Value::null_key_for_type(DataType::BIGINT);
  CHECK(skr.start == null_key);
  CHECK(skr.end == KeyCodecs::inclusive_upper_bound(null_key));
  CHECK(!skr.is_empty());
}

TEST(KeyRangeNull, ValueLowerBoundExcludesNullNaturally) {
  auto range = KeyRange::range(Value(1), Value(10));
  CHECK(!range.includes_null());
  CHECK(range.null_scope() == NullScope::EXCLUDE);
  CHECK(!range.contains(Value()));

  auto from = KeyRange::from(Value(1));
  CHECK(!from.includes_null());
  CHECK(!from.contains(Value()));
}

TEST(KeyRangeNull, NullAwareAlgebraFollowsKeys) {
  // intersect：all ∩ non_null = non_null
  auto both = KeyRange::all(DataType::BIGINT)
                  .intersect(KeyRange::non_null(DataType::BIGINT));
  CHECK(!both.includes_null());
  CHECK(both.contains(Value(0)));

  // complement(non_null) = 只有 NULL
  auto complement = KeyRange::non_null(DataType::BIGINT).complement();
  CHECK_EQ(complement.size(), 1);
  if (complement.size() == 1) {
    CHECK(complement[0].null_scope() == NullScope::ONLY);
    CHECK(complement[0].contains(Value()));
    CHECK(!complement[0].contains(Value(0)));
  }

  // unite(non_null, null_only) = all
  auto united = KeyRange::non_null(DataType::BIGINT)
                    .unite(KeyRange::null_only(DataType::BIGINT));
  CHECK_EQ(united.size(), 1);
  if (united.size() == 1) {
    CHECK(united[0].includes_null());
    CHECK(united[0].contains(Value(0)));
  }
}

TEST(KeyRangeNull, ExcludingNullKeepsAllValuesForEveryFamily) {
  auto strings = KeyRange::non_null(DataType::VARCHAR);
  CHECK(!strings.includes_null());
  CHECK(strings.contains(Value(std::string(""))));
  CHECK(strings.contains(Value(std::string("zzz"))));

  auto booleans = KeyRange::non_null(DataType::BOOLEAN);
  CHECK(booleans.contains(Value(false)));
  CHECK(booleans.contains(Value(true)));
  CHECK(!booleans.includes_null());

  auto dates = KeyRange::non_null(DataType::DATE);
  CHECK(dates.contains(Value::date(0)));
  CHECK(!dates.includes_null());
}

TEST(KeyRangeNull, OptimizerStyleUsage) {
  const DataType type = DataType::BIGINT;

  struct Case {
    KeyRange range;
    bool contains_5;
    bool contains_4;
    bool contains_6;
  };
  const Case cases[] = {
      {KeyRange::gt(Value(5)), false, false, true},
      {KeyRange::ge(Value(5)), true, false, true},
      {KeyRange::lt(Value(5)), false, true, false},
      {KeyRange::le(Value(5)), true, true, false},
      {KeyRange::eq(Value(5)), true, false, false},
  };
  for (const auto &item : cases) {
    CHECK(!item.range.includes_null()); // 比较谓词都不含 NULL
    CHECK(!item.range.contains(Value()));
    CHECK_EQ(item.range.contains(Value(5)), item.contains_5);
    CHECK_EQ(item.range.contains(Value(4)), item.contains_4);
    CHECK_EQ(item.range.contains(Value(6)), item.contains_6);
  }

  // lt/le 的下界无界，但工厂显式排除了 NULL（与裸 to(v) 不同）
  CHECK(!KeyRange::lt(Value(5)).includes_null());
  CHECK(!KeyRange::le(Value(5)).includes_null());
  CHECK(!KeyRange::to(Value(5)).includes_null() == false); // to(5) 含 NULL

  // 参数是 NULL：比较永不成立 → 空集
  CHECK(KeyRange::gt(Value()).is_empty());
  CHECK(KeyRange::le(Value()).is_empty());
  CHECK(KeyRange::eq(Value(), type).null_scope() == NullScope::ONLY);

  // IS NULL / IS NOT NULL / 全表
  CHECK(KeyRange::null_only(type).null_scope() == NullScope::ONLY);
  CHECK(KeyRange::non_null(type).null_scope() == NullScope::EXCLUDE);
  CHECK(KeyRange::all(type).null_scope() == NullScope::INCLUDE);
}

TEST(KeyRangeNull, NullAsExplicitHighBoundIsEmpty) {
  // 没有值比 NULL 更小 → to(NULL) 是空集；range(a, NULL) 同理
  CHECK(KeyRange::to(Value()).is_empty());
  CHECK(KeyRange::range(Value(1), Value()).is_empty());
}

// ============================================================
// 三条不变量（压平的正确性保证）
// ============================================================

TEST(KeyRangeInvariant, SuccessorIsUniversalAndSafe) {
  const DataType types[] = {DataType::BIGINT, DataType::VARCHAR,
                            DataType::BOOLEAN, DataType::DATE,
                            DataType::DATETIME};
  const std::string strings[] = {"", "a", "zz", std::string("\xff\xff", 2),
                                 std::string("a\0b", 3)};

  for (DataType type : types) {
    // 采样若干值（不同类型用不同样本）
    std::vector<Value> samples;
    if (is_string(type)) {
      for (const auto &s : strings)
        samples.push_back(Value(s));
    } else if (type == DataType::BOOLEAN) {
      samples.push_back(Value(false));
      samples.push_back(Value(true));
    } else {
      samples.push_back(Value::bigint(0));
      samples.push_back(Value::bigint(INT64_MAX));
      samples.push_back(Value::bigint(INT64_MIN));
    }

    for (const auto &v : samples) {
      const Key k = v.to_key();
      const Key next = KeyCodecs::inclusive_upper_bound(k);

      // (a) next 严格大于 k，且不是合法编码（不会被解析成某个值）
      CHECK(k < next);
      CHECK(!KeyCodecs::is_valid_key(next, type));

      // (b) next 不越出本族（首字节仍是本族 tag，且小于族上界）
      CHECK(!next.empty());
      CHECK(next[0] == k[0]);
      CHECK(next < Value::upper_key_for_type(type));

      // (c) 没有"合法 key"落在两者之间：扫一遍同族样本确认
      for (const auto &other : samples) {
        const Key ok = other.to_key();
        if (ok == k)
          continue;
        CHECK(!((k < ok) && (ok < next)));
      }
    }
  }
}

TEST(KeyRangeInvariant, FlattenMatchesContainsSemantics) {
  const Value probes[] = {Value(),         Value(-5), Value(0),
                          Value(5),        Value(10), Value(INT64_MAX),
                          Value(INT64_MIN)};
  const DataType type = DataType::BIGINT;

  const KeyRange ranges[] = {
      KeyRange::all(type),
      KeyRange::non_null(type),
      KeyRange::null_only(type),
      KeyRange::gt(Value(5)),
      KeyRange::ge(Value(5)),
      KeyRange::lt(Value(5)),
      KeyRange::le(Value(5)),
      KeyRange::eq(Value(5)),
      KeyRange::range(Value(1), Value(10)),
      KeyRange::closed(Value(1), Value(10)),
  };

  for (const auto &range : ranges) {
    const auto skr = range.to_str_key_range();
    for (const auto &probe : probes) {
      // 把 probe 编码成该类型下的 key（NULL 用该族的 NULL key）
      const Key key =
          probe.is_null() ? Value::null_key_for_type(type) : probe.to_key();
      const bool in_key_space = (skr.start <= key) && (key < skr.end);
      CHECK_EQ(range.contains(probe), in_key_space);
    }
  }
}

TEST(KeyRangeInvariant, EmptyAndPointFlattening) {
  const DataType type = DataType::BIGINT;

  // 空集：start >= end
  auto empty = KeyRange::empty(type).to_str_key_range();
  CHECK(!(empty.start < empty.end));
  CHECK(empty.is_empty());

  // 单点 [v, v]：start = key(v)，end = key(v)+0x00
  const Value v(5);
  auto point = KeyRange::closed(v, v).to_str_key_range();
  const Key kv = v.to_key();
  CHECK(point.start == kv);
  CHECK(point.end == KeyCodecs::inclusive_upper_bound(kv));
  CHECK(!point.is_empty());

  // 整数的 point() 工厂同样成立
  auto point2 = KeyRange::point(Value(5)).to_str_key_range();
  CHECK_EQ(point2.start, point.start);
  CHECK_EQ(point2.end, point.end);
}

TEST(KeyRangeNull, ComplementOfNullOnlyIsExactlyNonNull) {
  // complement(只有 NULL) 应该就是"只有非 NULL"，不应多出退化区间
  auto parts = KeyRange::null_only(DataType::BIGINT).complement();
  CHECK_EQ(parts.size(), 1);
  if (parts.size() == 1) {
    CHECK(parts[0].null_scope() == NullScope::EXCLUDE);
    CHECK(parts[0].contains(Value(0)));
    CHECK(!parts[0].contains(Value()));
  }

  // complement(all) = 空；complement(空) = all
  CHECK(KeyRange::all(DataType::BIGINT).complement()[0].is_empty());
  auto from_empty = KeyRange::empty(DataType::BIGINT).complement();
  CHECK_EQ(from_empty.size(), 1);
  if (from_empty.size() == 1) {
    CHECK(from_empty[0].is_all());
  }
}

TEST(KeyRangeNull, DegenerateUpperNullRangeIsEmpty) {
  // (-∞, NULL) 里没有任何值 → 空集
  CHECK(KeyRange::to(Value()).is_empty());
  CHECK(KeyRange::range(Value(1), Value()).is_empty());
  CHECK(KeyRange::range(Value(), Value()).is_empty());
  // 但 [NULL, NULL] 是单点，不是空集
  CHECK(!KeyRange::null_only(DataType::BIGINT).is_empty());
}

TEST(KeyRangeNull, SubtractKeepsNullSemantics) {
  const DataType type = DataType::BIGINT;

  // all - non_null = 只有 NULL
  auto null_part = KeyRange::all(type).subtract(KeyRange::non_null(type));
  CHECK_EQ(null_part.size(), 1);
  if (null_part.size() == 1) {
    CHECK(null_part[0].null_scope() == NullScope::ONLY);
    CHECK(null_part[0].contains(Value()));
    CHECK(!null_part[0].contains(Value(0)));
  }

  // non_null - null_only = non_null（两者不相交）
  auto still_values =
      KeyRange::non_null(type).subtract(KeyRange::null_only(type));
  CHECK_EQ(still_values.size(), 1);
  if (still_values.size() == 1) {
    CHECK(!still_values[0].includes_null());
    CHECK(still_values[0].contains(Value(0)));
  }

  // 有界区间减掉 NULL 区间：结果不变
  auto bounded = KeyRange::range(Value(1), Value(10));
  auto bounded_minus_null = bounded.subtract(KeyRange::null_only(type));
  CHECK_EQ(bounded_minus_null.size(), 1);
  if (bounded_minus_null.size() == 1) {
    CHECK(bounded_minus_null[0].equals(bounded));
  }
}

TEST(KeyRangeNull, UniteWithNullOnlyRestoresAll) {
  // 反向也成立：null_only ∪ non_null = all（无重叠、相邻）
  auto united = KeyRange::null_only(DataType::BIGINT)
                    .unite(KeyRange::non_null(DataType::BIGINT));
  CHECK_EQ(united.size(), 1);
  if (united.size() == 1) {
    CHECK(united[0].is_all());
    CHECK(united[0].includes_null());
    CHECK(united[0].contains(Value(0)));
  }
}

TEST(KeyRangeNull, ToStringIsHumanReadable) {
  CHECK_EQ(KeyRange::all(DataType::BIGINT).to_string(),
           std::string("(-∞, +∞)"));
  CHECK_EQ(KeyRange::empty(DataType::BIGINT).to_string(), std::string("∅"));
  CHECK_EQ(KeyRange::null_only(DataType::BIGINT).to_string(),
           std::string("{NULL}"));
  // 排除 NULL 的半无限区间
  CHECK_EQ(KeyRange::non_null(DataType::BIGINT).to_string(),
           std::string("(NULL, +∞)"));
  // 普通有界区间
  CHECK_EQ(KeyRange::range(Value(1), Value(10)).to_string(),
           std::string("[1, 10)"));
  CHECK_EQ(KeyRange::closed(Value(1), Value(10)).to_string(),
           std::string("[1, 10]"));
}
