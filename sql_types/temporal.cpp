// temporal.cpp
#include "temporal.h"

#include "field_type.h"

#include <cstdio>

namespace sql {
namespace temporal {
namespace {

bool is_digit(char c) { return c >= '0' && c <= '9'; }

// 读固定宽度数字；失败返回 false
bool read_digits(std::string_view s, size_t pos, size_t count, int64_t& out) {
  if (pos + count > s.size()) {
    return false;
  }
  int64_t value = 0;
  for (size_t i = 0; i < count; ++i) {
    const char c = s[pos + i];
    if (!is_digit(c)) {
      return false;
    }
    value = value * 10 + (c - '0');
  }
  out = value;
  return true;
}

bool read_separator(std::string_view s, size_t pos, char expected) {
  if (pos >= s.size()) {
    return false;
  }
  return s[pos] == expected;
}

std::string pad2(int64_t v) {
  char buf[8];
  std::snprintf(buf, sizeof(buf), "%02lld", static_cast<long long>(v));
  return buf;
}

std::string pad4(int64_t v) {
  char buf[8];
  std::snprintf(buf, sizeof(buf), "%04lld", static_cast<long long>(v));
  return buf;
}

}  // namespace

bool is_leap_year(int64_t year) {
  return (year % 4 == 0 && year % 100 != 0) || (year % 400 == 0);
}

int days_in_month(int64_t year, unsigned month) {
  static const int kDays[] = {31, 28, 31, 30, 31, 30,
                              31, 31, 30, 31, 30, 31};
  if (month < 1 || month > 12) {
    return 0;
  }
  if (month == 2 && is_leap_year(year)) {
    return 29;
  }
  return kDays[month - 1];
}

int64_t days_from_civil(int64_t y, unsigned m, unsigned d) {
  y -= m <= 2;
  const int64_t era = (y >= 0 ? y : y - 399) / 400;
  const unsigned yoe = static_cast<unsigned>(y - era * 400);
  const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
  const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return era * 146097 + static_cast<int64_t>(doe) - 719468;
}

void civil_from_days(int64_t days, int64_t& year, unsigned& month,
                     unsigned& day) {
  days += 719468;
  const int64_t era = (days >= 0 ? days : days - 146096) / 146097;
  const unsigned doe = static_cast<unsigned>(days - era * 146097);
  const unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
  const int64_t y = static_cast<int64_t>(yoe) + era * 400;
  const unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
  const unsigned mp = (5 * doy + 2) / 153;
  day = doy - (153 * mp + 2) / 5 + 1;
  month = mp + (mp < 10 ? 3 : -9);
  year = y + (month <= 2);
}

std::optional<int64_t> parse_date(std::string_view text) {
  int64_t year = 0;
  int64_t month = 0;
  int64_t day = 0;
  // "YYYY-MM-DD"
  if (text.size() != 10 || !read_digits(text, 0, 4, year) ||
      !read_separator(text, 4, '-') || !read_digits(text, 5, 2, month) ||
      !read_separator(text, 7, '-') || !read_digits(text, 8, 2, day)) {
    return std::nullopt;
  }
  if (month < 1 || month > 12 || day < 1 ||
      day > days_in_month(year, static_cast<unsigned>(month))) {
    return std::nullopt;
  }
  const int64_t days =
      days_from_civil(year, static_cast<unsigned>(month),
                      static_cast<unsigned>(day));
  if (days < kDateMinDays || days > kDateMaxDays) {
    return std::nullopt;
  }
  return days;
}

std::optional<int64_t> parse_time(std::string_view text) {
  int64_t hour = 0;
  int64_t minute = 0;
  int64_t second = 0;
  if (text.size() != 5 && text.size() != 8) {
    return std::nullopt;
  }
  if (!read_digits(text, 0, 2, hour) || !read_separator(text, 2, ':') ||
      !read_digits(text, 3, 2, minute)) {
    return std::nullopt;
  }
  if (text.size() == 8) {
    if (!read_separator(text, 5, ':') || !read_digits(text, 6, 2, second)) {
      return std::nullopt;
    }
  }
  if (hour < 0 || hour > 23 || minute < 0 || minute > 59 || second < 0 ||
      second > 59) {
    return std::nullopt;
  }
  return hour * 3600 + minute * 60 + second;
}

std::optional<int64_t> parse_datetime(std::string_view text) {
  // "YYYY-MM-DD HH:MM(:SS)" 或 ISO 的 "YYYY-MM-DDTHH:MM(:SS)"
  if (text.size() < 16) {
    return std::nullopt;
  }
  const auto days = parse_date(text.substr(0, 10));
  if (!days.has_value()) {
    return std::nullopt;
  }
  const char sep = text[10];
  if (sep != ' ' && sep != 'T' && sep != 't') {
    return std::nullopt;
  }
  const auto secs = parse_time(text.substr(11));
  if (!secs.has_value()) {
    return std::nullopt;
  }
  return *days * kSecondsPerDay + *secs;
}

std::string format_date(int64_t days) {
  int64_t year = 1970;
  unsigned month = 1;
  unsigned day = 1;
  civil_from_days(days, year, month, day);
  return pad4(year) + "-" + pad2(month) + "-" + pad2(day);
}

std::string format_time(int64_t seconds) {
  const int64_t hour = seconds / 3600;
  const int64_t minute = (seconds % 3600) / 60;
  const int64_t second = seconds % 60;
  return pad2(hour) + ":" + pad2(minute) + ":" + pad2(second);
}

std::string format_datetime(int64_t seconds) {
  int64_t days = seconds / kSecondsPerDay;
  int64_t rest = seconds % kSecondsPerDay;
  if (rest < 0) {          // 1970 年之前的负数秒
    rest += kSecondsPerDay;
    --days;
  }
  return format_date(days) + " " + format_time(rest);
}

}  // namespace temporal
}  // namespace sql
