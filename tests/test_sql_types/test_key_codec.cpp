// tests/test_sql_types/test_key_codec.cpp
//
// KeyCodecs 是存储格式的契约，这里覆盖：
//   - 各类型的编码/解码往返
//   - key 字节序 == 值逻辑序（字符串尤其重要）
//   - INT/BIGINT、VARCHAR/TEXT 的同族一致性
//   - NULL、空串、含 0x00 / 0xFF / '|' 的字符串
//   - 非法 key 的判定
#include "test_framework.h"
#include "sql_types/key.h"
#include "sql_types/value.h"

using namespace sql;

TEST(KeyCodec, Int64RoundTrip) {
    const int64_t samples[] = {0, 1, -1, 42, -42, 2147483647, -2147483648,
                               INT64_MAX, INT64_MIN, INT64_MAX - 1};
    for (int64_t v : samples) {
        Value original(v);
        Key k = original.to_key();
        CHECK(KeyCodecs::is_valid_key(k, DataType::BIGINT));
        Value back = Value::from_key(k, DataType::BIGINT);
        CHECK(!back.is_null());
        CHECK_EQ(back.as_int(), v);
    }
}

TEST(KeyCodec, Int64OrderingMatchesValueOrdering) {
    const int64_t samples[] = {INT64_MIN, -1000000000000LL, -2147483649LL,
                               -1, 0, 1, 2147483648LL, 1000000000000LL,
                               INT64_MAX};
    for (size_t i = 1; i < sizeof(samples) / sizeof(samples[0]); ++i) {
        Key prev = Value(samples[i - 1]).to_key();
        Key cur = Value(samples[i]).to_key();
        CHECK(prev < cur);          // 数值序 == 字节序
        CHECK(!(cur < prev));
    }
}

TEST(KeyCodec, StringOrderingMatchesValueOrdering) {
    // 之前的长度前缀编码会让 "b" < "aaaa"（长度优先），这是错的
    const std::string samples[] = {
        "", std::string("\x01"), "a", std::string("a\0", 2),
        std::string("a\0b", 3), "aa", "aaaa", "ab", "a|b", "b", "ba", "z",
        std::string("\x7f"), std::string("\xff")};
    for (size_t i = 1; i < sizeof(samples) / sizeof(samples[0]); ++i) {
        Key prev = Value(samples[i - 1]).to_key();
        Key cur = Value(samples[i]).to_key();
        CHECK(prev < cur);
    }
}

TEST(KeyCodec, StringRoundTripPreservesBytes) {
    const std::string samples[] = {
        "", "hello", "a|b", "a,b:c", std::string("a\0b", 3),
        std::string("\0", 1), std::string("\xff\xfe", 2),
        std::string("中文 UTF-8"), std::string(300, 'x')};
    for (const auto& s : samples) {
        Key k = Value(s).to_key();
        CHECK(KeyCodecs::is_valid_key(k, DataType::VARCHAR));
        Value back = Value::from_key(k, DataType::VARCHAR);
        CHECK(!back.is_null());
        CHECK_EQ(back.as_str(), s);
        CHECK_EQ(back.as_str().size(), s.size());
    }
}

TEST(KeyCodec, TextAndVarcharKeepTheirOwnType) {
    Key text_key = Value::text("hi").to_key();
    Key varchar_key = Value(std::string("hi")).to_key();

    // 同一族 -> 同一个 key
    CHECK_EQ(text_key, varchar_key);

    // 解码时保留调用方要求的逻辑类型
    CHECK(Value::from_key(text_key, DataType::TEXT).type() == DataType::TEXT);
    CHECK(Value::from_key(text_key, DataType::VARCHAR).type() ==
          DataType::VARCHAR);
}

TEST(KeyCodec, IntAndBigintShareOneEncoding) {
    Value i(7);                            // INT
    Value b(static_cast<int64_t>(7));      // BIGINT

    CHECK(i == b);                         // 值是相等的（同族）
    CHECK_EQ(i.to_key(), b.to_key());      // key 也必须相等，否则索引对不上
    CHECK_EQ(Value::from_key(i.to_key(), DataType::INT).as_int(), 7);
    CHECK_EQ(Value::from_key(i.to_key(), DataType::BIGINT).as_int(), 7);
}

TEST(KeyCodec, BoolRoundTrip) {
    for (bool v : {false, true}) {
        Key k = Value(v).to_key();
        CHECK(KeyCodecs::is_valid_key(k, DataType::BOOLEAN));
        Value back = Value::from_key(k, DataType::BOOLEAN);
        CHECK(!back.is_null());
        CHECK_EQ(back.as_bool(), v);
    }
    CHECK(Value(false).to_key() < Value(true).to_key());
}

TEST(KeyCodec, NullHasItsOwnKeySlot) {
    Value null_value;
    // 不带列类型：NULL 用 kTagNull 编码（[0x00][0x00]），排序在所有值之前
    const Key typeless = null_value.to_key();
    CHECK_EQ(typeless.size(), 2);
    CHECK_EQ(static_cast<uint8_t>(typeless[0]), KeyCodecs::kTagNull);
    CHECK_EQ(static_cast<uint8_t>(typeless[1]), KeyCodecs::kNullFlag);
    CHECK(typeless < Value(1).to_key());
    CHECK(typeless < Value(std::string("")).to_key());

    // 带列类型：NULL 用本族的 tag 编码，正好等于该族的最小 key
    const Key typed_int = null_value.to_key(DataType::INT);
    CHECK_EQ(typed_int, Value::min_key_for_type(DataType::INT));
    CHECK_EQ(typed_int.size(), 2);
    CHECK_EQ(static_cast<uint8_t>(typed_int[0]), KeyCodecs::kTagInt64);

    const Key typed_str = null_value.to_key(DataType::VARCHAR);
    CHECK_EQ(typed_str, Value::min_key_for_type(DataType::VARCHAR));
    CHECK_EQ(static_cast<uint8_t>(typed_str[0]), KeyCodecs::kTagString);

    // NULL 必须在全族范围内，且排在本族所有值之前
    CHECK(typed_int < Value(INT64_MIN).to_key());
    CHECK(typed_str < Value(std::string("")).to_key());
    CHECK(typed_int < Value::upper_key_for_type(DataType::INT));

    // 解码：两种 NULL key 都能解回 NULL
    CHECK(Value::from_key(typeless, DataType::INT).is_null());
    CHECK(Value::from_key(typed_int, DataType::INT).is_null());
    CHECK(KeyCodecs::is_valid_key(typed_int, DataType::INT));
    CHECK(KeyCodecs::is_valid_key(typeless, DataType::INT));
    CHECK(Value::from_key("", DataType::INT).is_null());
    CHECK(!KeyCodecs::is_valid_key("", DataType::INT));
}

TEST(KeyCodec, TemporalTypesShareInt64Encoding) {
    // DATE/TIME/DATETIME 与整数同族：编码一致、可按字节序排序
    const Key date_key = Value::date(19000).to_key();
    const Key datetime_key = Value::datetime(19000).to_key();
    CHECK_EQ(date_key, datetime_key);
    CHECK(KeyCodecs::less(Value::date(100), Value::date(200)));
    CHECK(KeyCodecs::less(Value::datetime(-5), Value::datetime(5)));

    // 解码时保留逻辑类型
    CHECK(Value::from_key(date_key, DataType::DATE).is_date());
    CHECK(Value::from_key(datetime_key, DataType::DATETIME).is_datetime());

    // 时间类型与整型属于同一编码族
    CHECK_EQ(KeyCodecs::tag_of(DataType::DATE), KeyCodecs::kTagInt64);
}

TEST(KeyCodec, RejectsMalformedKeys) {
    // 族标记不匹配
    CHECK(!KeyCodecs::is_valid_key(Value(1).to_key(), DataType::VARCHAR));
    CHECK(!KeyCodecs::is_valid_key(Value("x").to_key(), DataType::INT));
    // 长度不足
    CHECK(!KeyCodecs::is_valid_key(std::string("\x01", 1), DataType::INT));
    CHECK(!KeyCodecs::is_valid_key(std::string("\x01\x00\x00", 3),
                                   DataType::INT));
    // 字符串缺少终止符
    CHECK(!KeyCodecs::is_valid_key(std::string("\x02" "abc", 4),
                                   DataType::VARCHAR));
    // 字符串终止符之后还有内容
    CHECK(!KeyCodecs::is_valid_key(std::string("\x02" "a\x00" "z", 4),
                                   DataType::VARCHAR));
    // 不存在的类型族标记
    CHECK(!KeyCodecs::is_valid_key(std::string("\x09\x00", 2), DataType::INT));
    // 解码失败应返回 NULL 而不是抛异常
    CHECK(Value::from_key("garbage", DataType::INT).is_null());
}

TEST(KeyCodec, TypeBoundsCoverExactlyOneFamily) {
    // 字符串族的上下界把所有字符串 key 包住，且不含整数 key
    const Key slo = Value::min_key_for_type(DataType::VARCHAR);
    const Key shi = Value::upper_key_for_type(DataType::VARCHAR);
    CHECK(slo < Value(std::string("")).to_key());
    CHECK(Value(std::string("")).to_key() < shi);
    CHECK(Value(std::string("zzzz")).to_key() < shi);
    CHECK(Value(1).to_key() < slo);            // 整数族的 key 更小

    const Key ilo = Value::min_key_for_type(DataType::INT);
    const Key ihi = Value::upper_key_for_type(DataType::INT);
    CHECK(ilo < Value(static_cast<int64_t>(INT64_MIN)).to_key());
    CHECK(Value(static_cast<int64_t>(INT64_MAX)).to_key() < ihi);
    CHECK(ilo < Value(1).to_key());
    CHECK(Value(1).to_key() < ihi);
    CHECK(Value(std::string("a")).to_key() >= ihi);

    // INT 与 BIGINT 是同一族
    CHECK_EQ(ilo, Value::min_key_for_type(DataType::BIGINT));
    CHECK_EQ(ihi, Value::upper_key_for_type(DataType::BIGINT));
}

TEST(KeyCodec, InclusiveUpperBoundIsJustAboveValue) {
    Value v(std::string("a"));
    const Key k = v.to_key();
    const Key bound = KeyCodecs::inclusive_upper_bound(k);
    CHECK(k < bound);
    // [k, bound) 只包含 k 自己
    CHECK(!(Value(std::string("a\0", 2)).to_key() < bound));
    CHECK(!(Value(std::string("ab")).to_key() < bound));
}
