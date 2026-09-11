// tests/test_sql_types/test_temporal.cpp
//
// 日期/时间类型的解析、格式化、取值范围与存储往返。
#include "test_framework.h"
#include "sql_types/key.h"
#include "sql_types/row.h"
#include "sql_types/schema.h"
#include "sql_types/sql_truth.h"
#include "sql_types/temporal.h"
#include "sql_types/value.h"

using namespace sql;

TEST(Temporal, DaysFromCivilKnownValues) {
    CHECK_EQ(temporal::days_from_civil(1970, 1, 1), 0);
    CHECK_EQ(temporal::days_from_civil(1970, 1, 2), 1);
    CHECK_EQ(temporal::days_from_civil(1969, 12, 31), -1);
    CHECK_EQ(temporal::days_from_civil(2000, 3, 1), 11017);
    CHECK_EQ(temporal::days_from_civil(2024, 2, 29), 19782);

    int64_t year = 0;
    unsigned month = 0;
    unsigned day = 0;
    temporal::civil_from_days(0, year, month, day);
    CHECK_EQ(year, 1970);
    CHECK_EQ(month, 1u);
    CHECK_EQ(day, 1u);

    temporal::civil_from_days(-1, year, month, day);
    CHECK_EQ(year, 1969);
    CHECK_EQ(month, 12u);
    CHECK_EQ(day, 31u);
}

TEST(Temporal, LeapYearRules) {
    CHECK(temporal::is_leap_year(2024));
    CHECK(!temporal::is_leap_year(2023));
    CHECK(!temporal::is_leap_year(1900));   // 百年不闰
    CHECK(temporal::is_leap_year(2000));    // 四百年再闰
    CHECK_EQ(temporal::days_in_month(2024, 2), 29);
    CHECK_EQ(temporal::days_in_month(2023, 2), 28);
    CHECK_EQ(temporal::days_in_month(2024, 12), 31);
}

TEST(Temporal, ParseAndFormatDate) {
    const auto days = temporal::parse_date("2024-02-29");
    CHECK(days.has_value());
    if (days.has_value()) {
        CHECK_EQ(temporal::format_date(*days), std::string("2024-02-29"));
        CHECK_EQ(*days, temporal::days_from_civil(2024, 2, 29));
    }

    CHECK(!temporal::parse_date("2023-02-29").has_value());  // 非闰年
    CHECK(!temporal::parse_date("2023-13-01").has_value());
    CHECK(!temporal::parse_date("2023-00-10").has_value());
    CHECK(!temporal::parse_date("2023-01-32").has_value());
    CHECK(!temporal::parse_date("23-01-01").has_value());
    CHECK(!temporal::parse_date("2023/01/01").has_value());
    CHECK(!temporal::parse_date("").has_value());
}

TEST(Temporal, ParseAndFormatTime) {
    const auto seconds = temporal::parse_time("13:45:07");
    CHECK(seconds.has_value());
    if (seconds.has_value()) {
        CHECK_EQ(*seconds, 13 * 3600 + 45 * 60 + 7);
        CHECK_EQ(temporal::format_time(*seconds), std::string("13:45:07"));
    }
    // HH:MM 也接受，秒补 0
    const auto short_form = temporal::parse_time("01:02");
    CHECK(short_form.has_value());
    if (short_form.has_value()) {
        CHECK_EQ(*short_form, 3720);
        CHECK_EQ(temporal::format_time(*short_form), std::string("01:02:00"));
    }

    CHECK(!temporal::parse_time("24:00:00").has_value());
    CHECK(!temporal::parse_time("12:60:00").has_value());
    CHECK(!temporal::parse_time("12:00:60").has_value());
    CHECK(!temporal::parse_time("1:00").has_value());
}

TEST(Temporal, ParseAndFormatDatetime) {
    const auto seconds = temporal::parse_datetime("2024-02-29 13:45:07");
    CHECK(seconds.has_value());
    if (seconds.has_value()) {
        CHECK_EQ(*seconds,
                 temporal::days_from_civil(2024, 2, 29) * kSecondsPerDay +
                     13 * 3600 + 45 * 60 + 7);
        CHECK_EQ(temporal::format_datetime(*seconds),
                 std::string("2024-02-29 13:45:07"));
    }

    // ISO 风格的分隔符
    CHECK(temporal::parse_datetime("2024-02-29T13:45:07").has_value());
    // 1970 之前（负秒）格式化要正确
    CHECK_EQ(temporal::format_datetime(-1), std::string("1969-12-31 23:59:59"));
    CHECK(!temporal::parse_datetime("2024-02-29").has_value());
    CHECK(!temporal::parse_datetime("2024-02-29X13:45:07").has_value());
}

TEST(Temporal, ValueRoundTripThroughTextAndKey) {
    Value date = Value::from_string("2024-02-29", DataType::DATE);
    CHECK(date.is_date());
    CHECK_EQ(date.to_string(), std::string("2024-02-29"));

    Value time = Value::from_string("13:45:07", DataType::TIME);
    CHECK(time.is_time());
    CHECK_EQ(time.to_string(), std::string("13:45:07"));

    Value dt = Value::from_string("2024-02-29 13:45:07", DataType::DATETIME);
    CHECK(dt.is_datetime());
    CHECK_EQ(dt.to_string(), std::string("2024-02-29 13:45:07"));

    // 时间类型与整数同族：key 往返后仍是时间类型
    const Key key = dt.to_key();
    const Value back = Value::from_key(key, DataType::DATETIME);
    CHECK(back.is_datetime());
    CHECK_EQ(back.as_int(), dt.as_int());
}

TEST(Temporal, FromStringRejectsBadInput) {
    bool threw = false;
    try {
        (void)Value::from_string("2024-02-30", DataType::DATE);
    } catch (const std::invalid_argument&) {
        threw = true;
    }
    CHECK(threw);

    threw = false;
    try {
        (void)Value::from_string("25:00", DataType::TIME);
    } catch (const std::invalid_argument&) {
        threw = true;
    }
    CHECK(threw);
}

TEST(Temporal, OrderingFollowsTimeline) {
    const Value early = Value::from_string("1969-12-31", DataType::DATE);
    const Value late = Value::from_string("1970-01-01", DataType::DATE);
    CHECK(KeyCodecs::less(early, late));
    CHECK(sql_less(early, late) == Truth::TRUE);
}

// ============================================================
// 时间列在 schema/Row 中的取值范围与往返
// ============================================================

TEST(Temporal, SchemaValidatesTemporalColumns) {
    TableSchema schema;
    schema.set_name(Identifier("events"));
    schema.add_column(Identifier("id"), DataType::INT, true, false);
    schema.add_column(Identifier("on_date"), DataType::DATE, false, true);
    schema.add_column(Identifier("at_time"), DataType::TIME, false, true);
    schema.add_column(Identifier("created"), DataType::DATETIME, false, true);

    auto ok = row()
                  .set(Identifier("id"), 1)
                  .set(Identifier("on_date"),
                       Value::from_string("2024-02-29", DataType::DATE))
                  .set(Identifier("at_time"),
                       Value::from_string("09:30:00", DataType::TIME))
                  .set(Identifier("created"),
                       Value::from_string("2024-02-29 09:30:00",
                                          DataType::DATETIME))
                  .build(schema);
    CHECK(ok.has_value());

    if (ok.has_value()) {
        const std::string data = ok->serialize(schema);
        auto restored = Row::deserialize(data, schema);
        CHECK(restored.has_value());
        if (restored.has_value()) {
            CHECK_EQ((*restored)[1].to_string(), std::string("2024-02-29"));
            CHECK_EQ((*restored)[2].to_string(), std::string("09:30:00"));
            CHECK_EQ((*restored)[3].to_string(),
                     std::string("2024-02-29 09:30:00"));
        }
    }

    // DATE 值写进 DATETIME 列：单位不同，必须显式转换 -> 类型不匹配
    auto mismatch = row()
                        .set(Identifier("id"), 2)
                        .set(Identifier("created"), Value::date(1))
                        .build(schema);
    CHECK(!mismatch.has_value());
    CHECK(mismatch.error() == SchemaError::COLUMN_TYPE_MISMATCH);
}

TEST(Temporal, NullTemporalColumnRoundTrip) {
    TableSchema schema;
    schema.set_name(Identifier("events"));
    schema.add_column(Identifier("id"), DataType::INT, true, false);
    schema.add_column(Identifier("on_date"), DataType::DATE, false, true);

    auto built = row().set(Identifier("id"), 1).build(schema);
    CHECK(built.has_value());
    if (!built.has_value()) {
        return;
    }
    CHECK((*built)[1].is_null());

    const std::string data = built->serialize(schema);
    auto restored = Row::deserialize(data, schema);
    CHECK(restored.has_value());
    if (restored.has_value()) {
        CHECK((*restored)[1].is_null());
    }
}
