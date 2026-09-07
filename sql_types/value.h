// value.h
#pragma once

#include <string>
#include "field_type.h"

namespace sql {

using Key=std::string;

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
  Value(const Value& other) noexcept {
    if (this != &other) {
      type_ = other.type_;
      if(is_int()){
        int_val_ = other.int_val_;
      }else if(is_string()){
        str_val_ = other.str_val_;
      }else if(is_bool()){
        bool_val_ = other.bool_val_;
      }else{
        int_val_ = other.int_val_; 
      }
    }
    
  }
  Value(Value&& other) noexcept
    :  {
    if(this != &other){
      type_ = other.type_;
      if(is_int()){
        int_val_ = other.int_val_;
      }else if(is_string()){
        str_val_ = std::move(other.str_val_);
      }else if(is_bool()){
        bool_val_ = other.bool_val_;
      }else{
        int_val_ = other.int_val_; 
      }
    }
  }
  Value& operator=(const Value& other) noexcept {
    if (this != &other) {
      type_ = other.type_;
      if(is_int()){
        int_val_ = other.int_val_;
      }else if(is_string()){
        str_val_ = other.str_val_;
      }else if(is_bool()){
        bool_val_ = other.bool_val_;
      }else{
        int_val_ = other.int_val_; 
      }
    }
    return *this;
  }
  Value& operator=(Value&& other) noexcept {
    if(this != &other){
      if(is_int()){
        int_val_ = other.int_val_;
      }else if(is_string()){
        str_val_ = std::move(other.str_val_);
      }else if(is_bool()){
        bool_val_ = other.bool_val_;
      }else{
        int_val_ = other.int_val_; 
      }
    }
  }

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
  // to 文本字符串
  std::string to_string() const;
  static Value from_string(const std::string& str, DataType type);

  // to 二进制key
  Key to_key() const;
  static Value from_key(const Key& key, DataType type);

 private:
  DataType type_;
  int64_t int_val_ = 0;
  std::string str_val_;
  bool bool_val_ = false;
};

}  // namespace sql