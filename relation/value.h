// value.h
#pragma once

#include <optional>
#include <string>
#include <variant>

#include "types.h"

namespace sql {

// ============================================================
// SQL 值类型
// ============================================================
class Value {
 public:
  // ----- 构造 -----
  Value() : type_(DataType::NULL_TYPE) {}
  explicit Value(int v) : type_(DataType::INT), int_val_(v) {}
  explicit Value(long long v) : type_(DataType::INT), int_val_(v) {}
  explicit Value(const std::string& v)
      : type_(DataType::VARCHAR), str_val_(v) {}
  explicit Value(const char* v) : type_(DataType::VARCHAR), str_val_(v) {}
  explicit Value(bool v) : type_(DataType::BOOLEAN), bool_val_(v) {}

  // ----- 拷贝/移动 -----
  Value(const Value& other) = default;
  Value(Value&& other) = default;
  Value& operator=(const Value& other) = default;
  Value& operator=(Value&& other) = default;

  // ----- 类型检查 -----
  DataType type() const { return type_; }
  bool is_null() const { return type_ == DataType::NULL_TYPE; }
  bool is_int() const {
    return type_ == DataType::INT || type_ == DataType::BIGINT;
  }
  bool is_string() const {
    return type_ == DataType::VARCHAR || type_ == DataType::TEXT;
  }
  bool is_bool() const { return type_ == DataType::BOOLEAN; }

  // ----- 值获取 -----
  int64_t int_val() const { return int_val_; }
  const std::string& str_val() const { return str_val_; }
  bool bool_val() const { return bool_val_; }

  // ----- 比较 -----
  bool operator==(const Value& other) const;
  bool operator!=(const Value& other) const { return !(*this == other); }
  bool operator<(const Value& other) const;
  bool operator>(const Value& other) const;
  bool operator<=(const Value& other) const;
  bool operator>=(const Value& other) const;

  // ----- 转换 -----
  std::string to_string() const;
  static Value from_string(const std::string& str, DataType type);

 private:
  DataType type_;
  int64_t int_val_ = 0;
  std::string str_val_;
  bool bool_val_ = false;
};

}  // namespace sql