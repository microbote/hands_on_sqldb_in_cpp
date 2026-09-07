// value.cpp
#include "value.h"

namespace sql {

bool Value::operator==(const Value &other) const {
  if (type_ != other.type_)
    return false;

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

bool Value::operator<(const Value &other) const {
  if (type_ != other.type_)
    return false;

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

bool Value::operator>(const Value &other) const { return other < *this; }

bool Value::operator<=(const Value &other) const { return !(*this > other); }

bool Value::operator>=(const Value &other) const { return !(*this < other); }

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

Value Value::from_string(const std::string &str, DataType type) {
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

/*
这个实现的核心思路：
对于 INT/BIGINT（统一按 8 字节 int64 存储），把 int64 映射成无符号并翻转最高位
（即 uint64_t x = (uint64_t)v ^ (1ULL << 63)），再以大端序输出 8 字节。这样：

负数映射后始终小于 0 的正数映射；
同符号数字的大小关系保持不变；
8 字节定长且字典序 == 数值序。
*/
Key Value::to_key() const {
  switch (type_) {
  case DataType::INT:
  case DataType::BIGINT: {
    // 偏移 + 8 字节大端编码，保证数值顺序 == 字典序
    uint64_t encoded =
        static_cast<uint64_t>(static_cast<int64_t>(int_val_)) ^ (1ULL << 63);
    std::string out(sizeof(encoded), '\0');
    for (size_t i = 0; i < 8; ++i) {
      out[i] = static_cast<char>((encoded >> (56 - 8 * i)) & 0xFF);
    }
    return out;
  }

  case DataType::VARCHAR:
  case DataType::TEXT: {
    // 字符串：直接返回，但要转义分隔符（如果原始 string 里可能存在 '|' 等）
    // 这里简单返回原始字节，如果你用 `|` 做列分隔符且值可能包含 `|`，
    // 需要做转义（如 replace("|", "||") 或类似 Scheme）。
    return str_val_;
  }

  case DataType::BOOLEAN: {
    return bool_val_ ? "\x01" : "\x00";
  }

  default: // NULL / UNKNOWN
    return {};
  }
}

  Value Value::from_key(const Key& key, DataType type) {
  switch (type) {
    case DataType::INT:
    case DataType::BIGINT: {
      if (key.size() < 8) {
        return Value();  // NULL / 无效
      }
      
      uint64_t encoded = 0;
      for (size_t i = 0; i < 8; ++i) {
        encoded |= (static_cast<uint64_t>(static_cast<uint8_t>(key[i]))) << (56 - 8 * i);
      }
      
      // 还原符号位（反向偏移）
      int64_t val = static_cast<int64_t>(encoded ^ (1ULL << 63));
      return Value(val);
    }

    case DataType::VARCHAR:
    case DataType::TEXT: {
      // 字符串值直接返回（如果你实现了转义，这里需要反转义）
      // 如果你的 to_key() 对字符串有特殊 prefix 编码（比如长度前缀），
      // 这里需要匹配地解析回原始字符串
      return Value(key);
    }

    case DataType::BOOLEAN: {
      if (key.empty()) return Value();
      return Value(key[0] != '\0');
    }

    default:  // NULL / UNKNOWN
      return Value();
  }
}

} // namespace sql