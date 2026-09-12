// value.cpp
#include "value.h"

#include <charconv>
#include <limits>
#include <stdexcept>

#include "key.h"
#include "temporal.h"

namespace sql {

namespace {

bool is_null_text(std::string_view text) {
  return text == "\\N" || iequals_ascii(text, "null");
}

} // namespace

std::string Value::to_string() const {
  if (is_null()) {
    return "NULL";
  }
  if (is_string()) {
    return std::get<std::string>(storage_);
  }
  if (is_bool()) {
    return std::get<bool>(storage_) ? "true" : "false";
  }
  switch (type_) {
  case DataType::DATE:
    return temporal::format_date(std::get<int64_t>(storage_));
  case DataType::TIME:
    return temporal::format_time(std::get<int64_t>(storage_));
  case DataType::DATETIME:
    return temporal::format_datetime(std::get<int64_t>(storage_));
  default:
    return std::to_string(std::get<int64_t>(storage_));
  }
}

Value Value::from_string(const std::string &str, DataType type) {
  if (is_null_text(str)) {
    return Value();
  }

  switch (type) {
  case DataType::TINYINT:
  case DataType::SMALLINT:
  case DataType::INT:
  case DataType::BIGINT: {
    int64_t parsed = 0;
    if (!parse_int64_strict(str, parsed)) {
      throw std::invalid_argument("Invalid integer: " + str);
    }
    if (!can_represent(type, parsed)) {
      throw std::out_of_range(std::string("Value out of range for ") +
                              data_type_name(type) + ": " + str);
    }
    return Value(parsed, type);
  }
  case DataType::VARCHAR:
    return Value(str);
  case DataType::TEXT:
    return Value(std::string(str), DataType::TEXT);
  case DataType::BOOLEAN:
    if (iequals_ascii(str, "true") || str == "1") {
      return Value(true);
    }
    if (iequals_ascii(str, "false") || str == "0") {
      return Value(false);
    }
    throw std::invalid_argument("Invalid boolean: " + str);
  case DataType::DATE: {
    const auto days = temporal::parse_date(str);
    if (!days.has_value()) {
      throw std::invalid_argument("Invalid date: " + str);
    }
    return Value(*days, DataType::DATE);
  }
  case DataType::TIME: {
    const auto seconds = temporal::parse_time(str);
    if (!seconds.has_value()) {
      throw std::invalid_argument("Invalid time: " + str);
    }
    return Value(*seconds, DataType::TIME);
  }
  case DataType::DATETIME: {
    const auto seconds = temporal::parse_datetime(str);
    if (!seconds.has_value()) {
      throw std::invalid_argument("Invalid datetime: " + str);
    }
    return Value(*seconds, DataType::DATETIME);
  }
  default:
    throw std::invalid_argument("Cannot parse to type: " +
                                std::string(data_type_name(type)));
  }
}

Key Value::to_key() const {
  // 不带列类型：NULL 用无类型 NULL key（[0x00] 0x00）
  return KeyCodecs::to_key(*this);
}

Key Value::to_key(DataType column_type) const {
  return KeyCodecs::to_key(*this, column_type);
}

Value Value::from_key(const Key &key, DataType type) {
  return KeyCodecs::from_key(key, type);
}

Key Value::min_key_for_type(DataType type) {
  return KeyCodecs::min_key_for_type(type);
}

Key Value::upper_key_for_type(DataType type) {
  return KeyCodecs::upper_key_for_type(type);
}

Key Value::null_key_for_type(DataType type) {
  return KeyCodecs::null_key_for_type(type);
}

Key Value::first_value_key_for_type(DataType type) {
  return KeyCodecs::first_value_key(type);
}

} // namespace sql
