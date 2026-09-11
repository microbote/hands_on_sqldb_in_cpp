// temporal.h
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace sql {
namespace temporal {

// ============================================================
// 时间类型的值表示（与 field_type.h 的约定一致）：
//   DATE     -> 距 1970-01-01 的天数
//   TIME     -> 自 00:00:00 起的秒数 [0, 86399]
//   DATETIME -> Unix 秒（UTC；暂不支持时区）
//
// 文本格式（宽松支持几种常见写法）：
//   DATE     : "YYYY-MM-DD"
//   TIME     : "HH:MM[:SS]"
//   DATETIME : "YYYY-MM-DD[ T]HH:MM[:SS]"
// 解析失败返回 nullopt；不抛异常。
// ============================================================

std::optional<int64_t> parse_date(std::string_view text);
std::optional<int64_t> parse_time(std::string_view text);
std::optional<int64_t> parse_datetime(std::string_view text);

std::string format_date(int64_t days);
std::string format_time(int64_t seconds);
std::string format_datetime(int64_t seconds);

bool is_leap_year(int64_t year);
int days_in_month(int64_t year, unsigned month);

// 民用日期 <-> 天数（proleptic Gregorian）
int64_t days_from_civil(int64_t year, unsigned month, unsigned day);
void civil_from_days(int64_t days, int64_t& year, unsigned& month,
                     unsigned& day);

}  // namespace temporal
}  // namespace sql
