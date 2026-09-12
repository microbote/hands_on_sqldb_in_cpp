// sql_types/value.h
#pragma once

#include <cassert>
#include <functional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>

#include "field_type.h"

namespace sql {

using Key = std::string;

// ============================================================
// Value：SQL 逻辑层的值
//
// 存储策略（与存储层的 key 编码一致）：
//   - 所有整型（TINYINT/SMALLINT/INT/BIGINT）共用 int64_t，
//     Value 的逻辑类型统一为 BIGINT；逻辑宽度只存在于 schema。
//   - DATE/TIME/DATETIME 也用 int64_t（天 / 秒 / 秒）。
//   - 字符串用 std::string（SSO，无堆分配小字符串）。
//   - NULL 用 std::monostate。
//
// 关于比较（重要）：
//   本类只提供 C++ 语义的 **相等**（operator==，用于去重/容器）。
//   全序比较属于存储层，见 KeyCodecs::compare()；
//   SQL 的三值比较见 sql_truth.h 的 sql_compare()/sql_compare_op()。
//   NULL 参与 SQL 比较时既不是 TRUE 也不是 FALSE，而是 UNKNOWN，
//   因此不要把 operator== 当作 SQL 的 "="。
// ============================================================
class Value {
public:
  // ----- 构造 -----
  Value() noexcept : type_(DataType::NULL_TYPE), storage_(std::monostate{}) {}

  Value(int v) noexcept
      : type_(DataType::BIGINT), storage_(static_cast<int64_t>(v)) {}

  Value(int64_t v) noexcept : type_(DataType::BIGINT), storage_(v) {}

  Value(bool v) noexcept : type_(DataType::BOOLEAN), storage_(v) {}

  Value(const std::string &v)
      : type_(DataType::VARCHAR), storage_(std::string(v)) {}

  Value(std::string &&v) : type_(DataType::VARCHAR), storage_(std::move(v)) {}

  Value(const char *v)
      : type_(DataType::VARCHAR), storage_(std::string(v != nullptr ? v : "")) {
  }

  Value(std::string_view v)
      : type_(DataType::VARCHAR), storage_(std::string(v)) {}

  // 显式指定逻辑类型的字符串（VARCHAR / TEXT）
  Value(std::string v, DataType type) : type_(type), storage_(std::move(v)) {
    assert(sql::is_string(type_));
  }

  // 显式指定逻辑类型的整数（含整型宽度与时间类型）
  Value(int64_t v, DataType type) : type_(type), storage_(v) {
    assert(sql::is_integer(type_) || sql::is_temporal(type_) ||
           sql::is_boolean(type_));
  }

  // ----- 拷贝/移动：variant 自动处理，无需手写 -----
  Value(const Value &) = default;
  Value(Value &&) noexcept = default;
  Value &operator=(const Value &) = default;
  Value &operator=(Value &&) noexcept = default;
  ~Value() = default;

  // ----- 类型检查 -----
  DataType type() const noexcept { return type_; }

  bool is_null() const noexcept { return is_null_type(type_); }

  // 逻辑上是不是整数（整型 family）
  bool is_int() const noexcept { return is_integer(type_); }

  bool is_string() const noexcept { return sql::is_string(type_); }

  bool is_bool() const noexcept { return type_ == DataType::BOOLEAN; }

  bool is_numeric() const noexcept { return sql::is_numeric(type_); }

  bool is_temporal() const noexcept { return sql::is_temporal(type_); }
  bool is_date() const noexcept { return type_ == DataType::DATE; }
  bool is_time() const noexcept { return type_ == DataType::TIME; }
  bool is_datetime() const noexcept { return type_ == DataType::DATETIME; }

  // ----- 值获取（带安全检查）-----
  int64_t as_int() const {
    if (!holds_int64()) {
      throw std::runtime_error("Value is not stored as int64");
    }
    return std::get<int64_t>(storage_);
  }

  const std::string &as_str() const {
    if (!is_string()) {
      throw std::runtime_error("Value is not a string");
    }
    return std::get<std::string>(storage_);
  }

  const std::string &as_string() const { return as_str(); }

  bool as_bool() const {
    if (!is_bool()) {
      throw std::runtime_error("Value is not a boolean");
    }
    return std::get<bool>(storage_);
  }

  // ----- 不抛异常的获取 -----
  int64_t get_int(int64_t default_val = 0) const noexcept {
    return holds_int64() ? std::get<int64_t>(storage_) : default_val;
  }

  std::string_view get_str_view() const noexcept {
    return is_string() ? std::string_view(std::get<std::string>(storage_))
                       : std::string_view();
  }

  const std::string *get_str_ptr() const noexcept {
    return is_string() ? &std::get<std::string>(storage_) : nullptr;
  }

  bool get_bool(bool default_val = false) const noexcept {
    return is_bool() ? std::get<bool>(storage_) : default_val;
  }

  // C++ 语义的真值判断（NULL -> false）。
  // 注意：这不是 SQL 的三值逻辑，SQL 判断请用 sql_truth.h。
  bool is_truthy() const {
    if (holds_int64()) {
      return std::get<int64_t>(storage_) != 0;
    }
    if (is_bool()) {
      return std::get<bool>(storage_);
    }
    if (is_string()) {
      return !std::get<std::string>(storage_).empty();
    }
    return false; // NULL
  }

  // ----- 类型判断模板 -----
  template <typename T> bool is() const {
    if constexpr (std::is_same_v<T, int>) {
      return is_int();
    } else if constexpr (std::is_same_v<T, int64_t>) {
      return holds_int64();
    } else if constexpr (std::is_same_v<T, std::string>) {
      return is_string();
    } else if constexpr (std::is_same_v<T, bool>) {
      return is_bool();
    } else if constexpr (std::is_same_v<T, std::nullptr_t>) {
      return is_null();
    }
    return false;
  }

  // ----- 相等（C++ 容器语义；不是 SQL 的 =）-----
  // 同一 family 内相等：所有整型互通、VARCHAR/TEXT 互通。
  // NULL == NULL 为 true（去重/容器需要），SQL 层由 sql_compare 判定 UNKNOWN。
  bool operator==(const Value &other) const {
    if (is_null() || other.is_null()) {
      return is_null() && other.is_null();
    }
    if (is_temporal() || other.is_temporal()) {
      // DATE / TIME / DATETIME 单位不同，不做隐式换算（SQL 层负责提升）
      return type_ == other.type_ && holds_int64() && other.holds_int64() &&
             std::get<int64_t>(storage_) == std::get<int64_t>(other.storage_);
    }
    const auto lhs_class = get_type_family(type_);
    const auto rhs_class = get_type_family(other.type_);
    if (lhs_class != rhs_class) {
      return false;
    }
    switch (lhs_class) {
    case DataTypeFamily::INTEGER:
      return std::get<int64_t>(storage_) == std::get<int64_t>(other.storage_);
    case DataTypeFamily::BOOLEAN:
      return std::get<bool>(storage_) == std::get<bool>(other.storage_);
    case DataTypeFamily::STRING:
      return std::get<std::string>(storage_) ==
             std::get<std::string>(other.storage_);
    default:
      return false;
    }
  }

  bool operator!=(const Value &other) const { return !(*this == other); }

  // ----- 转换为字符串 -----
  std::string to_string() const;

  // ----- 解析字符串为值 -----
  static Value from_string(const std::string &str, DataType type);

  // ----- 静态工厂 -----
  static Value null() { return Value(); }
  static Value boolean(bool v) { return Value(v); }
  static Value integer(int64_t v) { return Value(v); }
  static Value bigint(int64_t v) { return Value(v, DataType::BIGINT); }
  static Value tinyint(int64_t v) { return Value(v, DataType::TINYINT); }
  static Value smallint(int64_t v) { return Value(v, DataType::SMALLINT); }
  static Value text(const std::string &v) {
    return Value(std::string(v), DataType::TEXT);
  }
  static Value varchar(const std::string &v) { return Value(v); }
  static Value date(int64_t days) { return Value(days, DataType::DATE); }
  static Value time(int64_t seconds) { return Value(seconds, DataType::TIME); }
  static Value datetime(int64_t seconds) {
    return Value(seconds, DataType::DATETIME);
  }

  // ----- 哈希（与 operator== 保持一致）-----
  size_t hash() const noexcept {
    if (is_null()) {
      return 0;
    }
    if (is_temporal()) {
      // 单位不同（DATE/TIME/DATETIME），混入类型避免同值不同型同哈希
      return std::hash<int64_t>()(std::get<int64_t>(storage_)) ^
             (static_cast<size_t>(type_) << 1);
    }
    if (is_bool()) {
      return std::hash<bool>()(std::get<bool>(storage_));
    }
    if (is_string()) {
      return std::hash<std::string>()(std::get<std::string>(storage_));
    }
    return std::hash<int64_t>()(std::get<int64_t>(storage_));
  }

  // ============================================================
  // Key 编码（存储层契约，见 key.h）
  // ============================================================

  // 不带列类型的编码：NULL 没有族信息，返回空串（NULL 需要列类型才能编码，
  // 见下面的重载）。
  Key to_key() const;

  // 带列类型的编码：存储层必须用这个版本（NULL 用它所属的列类型编码）。
  Key to_key(DataType column_type) const;

  static Value from_key(const Key &key, DataType type);

  static Key min_key_for_type(DataType type);
  static Key upper_key_for_type(DataType type);

  // 该类型的 NULL key（= 族最小值，也是 -∞ 的物理表示）
  static Key null_key_for_type(DataType type);

  // "第一个非 NULL 值"的前缀 key（[tag][0x01]），用于排除 NULL 的下界
  static Key first_value_key_for_type(DataType type);

private:
  // 存储里是否真的持有 int64（整型或时间）
  bool holds_int64() const noexcept {
    return is_integer(type_) || sql::is_temporal(type_);
  }

  static bool is_null_type(DataType t) {
    return t == DataType::NULL_TYPE || t == DataType::UNKNOWN_TYPE;
  }

  DataType type_;
  std::variant<std::monostate, int64_t, bool, std::string> storage_;
};

// 哈希支持
struct ValueHash {
  size_t operator()(const Value &v) const noexcept { return v.hash(); }
};

// key 空间的全序比较（存储层语义）：NULL 最小，其余按编码字节序。
// Value 本身不再重载 < / > 等操作符，避免把存储序误当作 SQL 比较。
struct ValueKeyLess {
  bool operator()(const Value &a, const Value &b) const {
    return a.to_key() < b.to_key();
  }
};

struct ValueKeyEqual {
  bool operator()(const Value &a, const Value &b) const {
    return a.to_key() == b.to_key();
  }
};

} // namespace sql
