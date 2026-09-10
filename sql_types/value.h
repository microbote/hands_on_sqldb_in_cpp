// sql_types/value.h
#pragma once

#include <cassert>
#include <cstring>
#include <functional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>

#include "field_type.h"

namespace sql {

using Key = std::string;

// ============================================================
// SQL 值类型 - 使用 union 优化内存
//
// 内存布局:
// - 标量类型 (int64_t, bool) 直接存储，无额外开销
// - 字符串类型使用 new 分配，避免 union 中的复杂对象
// - 支持 NULL 值
// ============================================================
class Value {
public:
  // ----- 构造 -----
  Value() noexcept : type_(DataType::NULL_TYPE), int_val_(0) {}

  explicit Value(int v) noexcept
      : type_(DataType::INT), int_val_(static_cast<int64_t>(v)) {}

  explicit Value(int64_t v) noexcept : type_(DataType::BIGINT), int_val_(v) {}

  explicit Value(const std::string &v)
      : type_(DataType::VARCHAR), str_ptr_(new std::string(v)) {}

  explicit Value(const char *v)
      : type_(DataType::VARCHAR), str_ptr_(new std::string(v)) {}

  // 显式指定字符串的逻辑类型（VARCHAR 或 TEXT）。
  // 取代早期的 Value(str, bool) 技巧：语义清晰，且不会和 bool 构造混淆。
  Value(std::string v, DataType type)
      : type_(type), str_ptr_(new std::string(std::move(v))) {
    assert(sql::is_string(type_));
  }

  explicit Value(bool v) noexcept : type_(DataType::BOOLEAN), bool_val_(v) {}

  // ----- 拷贝构造 -----
  Value(const Value &other) : type_(other.type_) {
    if (other.is_string()) {
      str_ptr_ = new std::string(*other.str_ptr_);
    } else {
      int_val_ = other.int_val_;  // 覆盖 union 的标量成员（含 bool）
    }
  }

  // ----- 移动构造：直接转移资源，源置为 NULL -----
  Value(Value &&other) noexcept : type_(other.type_) {
    if (other.is_string()) {
      str_ptr_ = other.str_ptr_;
      other.str_ptr_ = nullptr;
      other.type_ = DataType::NULL_TYPE;
    } else {
      int_val_ = other.int_val_;
      other.type_ = DataType::NULL_TYPE;
    }
  }

  // ----- 析构 -----
  ~Value() { cleanup(); }

  // ----- 拷贝赋值 -----
  Value &operator=(const Value &other) {
    if (this != &other) {
      // 如果都是字符串，可以直接赋值避免重新分配
      if (is_string() && other.is_string()) {
        *str_ptr_ = *other.str_ptr_;
        return *this;
      }

      // 否则需要清理后重新构造
      cleanup();
      type_ = other.type_;
      if (other.is_string()) {
        str_ptr_ = new std::string(*other.str_ptr_);
      } else {
        int_val_ = other.int_val_;
      }
    }
    return *this;
  }

  // ----- 移动赋值 -----
  Value &operator=(Value &&other) noexcept {
    if (this != &other) {
      cleanup();

      type_ = other.type_;
      if (other.is_string()) {
        str_ptr_ = other.str_ptr_;
        other.str_ptr_ = nullptr;
        other.type_ = DataType::NULL_TYPE;
      } else {
        int_val_ = other.int_val_;
        other.type_ = DataType::NULL_TYPE;
      }
    }
    return *this;
  }

  // ----- 类型检查 -----
  DataType type() const noexcept { return type_; }

  bool is_null() const noexcept {
    return type_ == DataType::NULL_TYPE || type_ == DataType::UNKNOWN_TYPE;
  }

  bool is_int() const noexcept {
    return type_ == DataType::INT || type_ == DataType::BIGINT;
  }

  bool is_string() const noexcept {
    return type_ == DataType::VARCHAR || type_ == DataType::TEXT;
  }

  bool is_bool() const noexcept { return type_ == DataType::BOOLEAN; }

  bool is_numeric() const noexcept { return is_int(); }

  // ----- 值获取（带安全检查）-----
  int64_t as_int() const {
    if (!is_int()) {
      throw std::runtime_error("Value is not an integer");
    }
    return int_val_;
  }

  const std::string &as_str() const {
    if (!is_string()) {
      throw std::runtime_error("Value is not a string");
    }
    return *str_ptr_;
  }

  const std::string &as_string() const { return as_str(); }

  bool as_bool() const {
    if (!is_bool()) {
      throw std::runtime_error("Value is not a boolean");
    }
    return bool_val_;
  }

  // ----- 安全的获取方式（不抛异常）-----
  int64_t get_int(int64_t default_val = 0) const noexcept {
    return is_int() ? int_val_ : default_val;
  }

  std::string_view get_str_view() const noexcept {
    return is_string() ? std::string_view(*str_ptr_) : std::string_view();
  }

  const std::string *get_str_ptr() const noexcept {
    return is_string() ? str_ptr_ : nullptr;
  }

  bool get_bool(bool default_val = false) const noexcept {
    return is_bool() ? bool_val_ : default_val;
  }

  // ----- 判断值是否相等（用于哈希等）-----
  bool is_truthy() const {
    if (is_bool()) {
      return bool_val_;
    }
    if (is_int()) {
      return int_val_ != 0;
    }
    if (is_string()) {
      return !str_ptr_->empty();
    }
    return false; // NULL
  }

  // ----- 类型转换 ----/
  explicit operator bool() const { return as_bool(); }
  explicit operator int64_t() const { return as_int(); }
  explicit operator std::string() const { return as_str(); }
  operator std::string_view() const { return get_str_view(); }

  // ----- 比较运算符 -----
  bool operator==(const Value &other) const {
    // 类型不同直接返回 false（除非都是 NULL）
    auto type_class = get_type_class(type_);
    auto other_class = get_type_class(other.type_);
    if (type_class != other_class) {
      // including NULL == NULL is true
      return false;
    }

    switch (type_class) {
    case DataTypeClass::INTEGER:
      return int_val_ == other.int_val_;
    case DataTypeClass::STRING:
      return *str_ptr_ == *other.str_ptr_;
    case DataTypeClass::BOOLEAN:
      return bool_val_ == other.bool_val_;
    case DataTypeClass::UNKNOWN_CLASS:
      return is_null() && other.is_null();
    default:
      return false;
    }
  }

  bool operator!=(const Value &other) const { return !(*this == other); }

  bool operator<(const Value &other) const {
    // NULL 值总是最小
    if (is_null()) {
      return !other.is_null();
    }
    if (other.is_null()) {
      return false;
    }
    auto type_class = get_type_class(type_);
    auto other_class = get_type_class(other.type_);
    // 类型不同时，按类型序号比较
    if (type_class != other_class) {
      return static_cast<int>(type_) < static_cast<int>(other.type_);
    }

    switch (type_) {
    case DataType::INT:
    case DataType::BIGINT:
      return int_val_ < other.int_val_;
    case DataType::VARCHAR:
    case DataType::TEXT:
      return *str_ptr_ < *other.str_ptr_;
    case DataType::BOOLEAN:
      return static_cast<int>(bool_val_) < static_cast<int>(other.bool_val_);
    default:
      return false;
    }
  }

  bool operator>(const Value &other) const { return other < *this; }

  bool operator<=(const Value &other) const { return !(other < *this); }

  bool operator>=(const Value &other) const { return !(*this < other); }

  // ----- 类型判断模板 -----
  template <typename T> bool is() const {
    if constexpr (std::is_same_v<T, int>) {
      return type_ == DataType::INT;
    } else if constexpr (std::is_same_v<T, int64_t>) {
      return is_int();
    } else if constexpr (std::is_same_v<T, std::string>) {
      return is_string();
    } else if constexpr (std::is_same_v<T, bool>) {
      return type_ == DataType::BOOLEAN;
    } else if constexpr (std::is_same_v<T, std::nullptr_t>) {
      return is_null();
    }
    return false;
  }

  // ----- 转换为字符串 -----
  std::string to_string() const {
    switch (type_) {
    case DataType::INT:
    case DataType::BIGINT:
      return std::to_string(int_val_);
    case DataType::VARCHAR:
    case DataType::TEXT:
      return *str_ptr_;
    case DataType::BOOLEAN:
      return bool_val_ ? "true" : "false";
    case DataType::NULL_TYPE:
      return "NULL";
    default:
      return "UNKNOWN";
    }
  }

  // ----- 解析字符串为值 -----
  static Value from_string(const std::string &str, DataType type) {
    // 处理 NULL（大小写不敏感："NULL" / "null" / "Null" / "\N"）
    if (str == "\\N" || iequals_ascii(str, "null")) {
      return Value();
    }

    switch (type) {
    case DataType::INT:
    case DataType::BIGINT:
      // 两个整数类型底层都是 int64_t，统一用 stoll，
      // 避免 stoi 在 32 位边界上溢出抛异常。
      return Value(std::stoll(str));
    case DataType::VARCHAR:
      return Value(str);
    case DataType::TEXT:
      return Value(str, DataType::TEXT);
    case DataType::BOOLEAN:
      if (iequals_ascii(str, "true") || str == "1") {
        return Value(true);
      }
      if (iequals_ascii(str, "false") || str == "0") {
        return Value(false);
      }
      throw std::invalid_argument("Invalid boolean: " + str);
    default:
      throw std::invalid_argument("Cannot parse to type: " +
                                  std::to_string(static_cast<int>(type)));
    }
  }

  // ----- 静态工厂方法 -----
  static Value null() { return Value(); }
  static Value boolean(bool v) { return Value(v); }
  static Value integer(int64_t v) { return Value(v); }
  static Value text(const std::string &v) { return Value(v, DataType::TEXT); }

  // ----- 获取字符串的哈希值 -----
  size_t hash() const noexcept {
    if (is_int()) {
      return std::hash<int64_t>()(int_val_);
    } else if (is_string()) {
      return std::hash<std::string>()(*str_ptr_);
    } else if (is_bool()) {
      return std::hash<bool>()(bool_val_);
    }
    return 0; // NULL 的哈希值
  }

  // to 二进制key
  Key to_key() const;

  static Value from_key(const Key &key, DataType type);

  // 类型最小值的 key
  static Key min_key_for_type(DataType type);

  // 类型最大值的 key
  static Key upper_key_for_type(DataType type);

private:
  // 类型枚举（由于 field_type.h 中的 DataType 已存在，我们复用它）
  DataType type_;

  // union 存储标量类型
  union {
    int64_t int_val_;      // 用于 INT, BIGINT
    bool bool_val_;        // 用于 BOOLEAN
    std::string *str_ptr_; // 指向字符串的指针
  };

  // 清理资源
  void cleanup() noexcept {
    if (is_string() && str_ptr_ != nullptr) {
      delete str_ptr_;
      str_ptr_ = nullptr;
    }
    type_ = DataType::NULL_TYPE;
  }

  // 深度比较辅助
  bool equals_as_ints(const Value &other) const noexcept {
    return int_val_ == other.int_val_;
  }

  bool equals_as_strings(const Value &other) const noexcept {
    return *str_ptr_ == *other.str_ptr_;
  }

  bool equals_as_bools(const Value &other) const noexcept {
    return bool_val_ == other.bool_val_;
  }
};

// 辅助：提供默认哈希支持
struct ValueHash {
  size_t operator()(const Value &v) const noexcept { return v.hash(); }
};

} // namespace sql
