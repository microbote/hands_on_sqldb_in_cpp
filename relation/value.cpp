// value.cpp
#include "value.h"

#include <iomanip>
#include <sstream>

namespace sql {

bool Value::operator==(const Value& other) const {
  if (type_ != other.type_) return false;

  switch (type_) {
    case DataType::INT:
    case DataType::BIGINT:
      return int_val_ == other.int_val_;
    case DataType::VARCHAR:
    case DataType::TEXT:
      return str_val_ == other.str_val_;
    case DataType::BOOLEAN:
      return bool_val_ == other.bool_val_;
    case DataType::NULL_TYPE:
      return true;
    default:
      return false;
  }
}

bool Value::operator<(const Value& other) const {
  if (type_ != other.type_) return false;

  switch (type_) {
    case DataType::INT:
    case DataType::BIGINT:
      return int_val_ < other.int_val_;
    case DataType::VARCHAR:
    case DataType::TEXT:
      return str_val_ < other.str_val_;
    case DataType::BOOLEAN:
      return bool_val_ < other.bool_val_;
    default:
      return false;
  }
}

bool Value::operator>(const Value& other) const { return other < *this; }

bool Value::operator<=(const Value& other) const { return !(*this > other); }

bool Value::operator>=(const Value& other) const { return !(*this < other); }

std::string Value::to_string() const {
  switch (type_) {
    case DataType::INT:
    case DataType::BIGINT:
      return std::to_string(int_val_);
    case DataType::VARCHAR:
    case DataType::TEXT:
      return str_val_;
    case DataType::BOOLEAN:
      return bool_val_ ? "true" : "false";
    case DataType::NULL_TYPE:
      return "NULL";
    default:
      return "UNKNOWN";
  }
}

Value Value::from_string(const std::string& str, DataType type) {
  if (str == "NULL") {
    return Value();
  }

  switch (type) {
    case DataType::INT:
    case DataType::BIGINT:
      return Value(std::stoll(str));
    case DataType::VARCHAR:
    case DataType::TEXT:
      return Value(str);
    case DataType::BOOLEAN:
      return Value(str == "true" || str == "1");
    default:
      return Value();
  }
}

}  // namespace sql