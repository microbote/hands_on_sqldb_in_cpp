// identifier.h
#pragma once

#include <string>
#include <string_view>
#include <functional>
#include <utility>
#include <fmt/format.h>

namespace sql {

//只是保存原始字符串，但是内部小写化比较。

class Identifier {
 public:
  // 默认构造
  Identifier() = default;

  // 从字符串构造 - 自动检测并去除引号
  explicit Identifier(std::string raw) {
    // 自动检测是否被引号包围
    if (raw.size() >= 2 && ((raw.front() == '"' && raw.back() == '"') || 
      (raw.front() == '\'' && raw.back() == '\''))) {
      quoted_ = true;
      name_ = raw.substr(1, raw.size() - 2);  // 去掉引号
    } else {
      quoted_ = false;
      name_ = std::move(raw);
    }
    // 创建小写副本（仅存储，不比较时用）
    normalized_ = make_lower(name_);
  }

  // 从 const char* 构造
  explicit Identifier(const char* name) 
      : Identifier(std::string((name != nullptr) ? name : "")) {}

  // 从 string_view 构造
  explicit Identifier(std::string_view name)
      : Identifier(std::string(name)) {}

  // ---- 访问器 ----
  const std::string& str() const { return name_; }
  const char* c_str() const { return name_.c_str(); }
  bool empty() const { return name_.empty(); }
  size_t size() const { return name_.size(); }

  // 小写版本 - O(1) 访问
  const std::string& lower() const { return normalized_; }
  std::string_view lower_view() const { return normalized_; }

  // 原始名称视图 - 避免拷贝
  std::string_view view() const { return name_; }

  // 是否带引号
  bool is_quoted() const { return quoted_; }

  // 展示（带引号形式）
  std::string display_name() const {
    if (quoted_) {
      return "\"" + name_ + "\"";
    }
    return name_;
  }

  // ==== 比较：直接用预计算的小写形式，避免重复转换 ====

  bool operator==(const Identifier& other) const {
    return normalized_ == other.normalized_;
  }

  bool operator!=(const Identifier& other) const {
    return !(*this == other);
  }

  bool operator<(const Identifier& other) const {
    return normalized_ < other.normalized_;
  }

  // 与字符串比较（仍会转换对方，但对方可能是临时转换）
  bool operator==(const std::string& other) const {
    return normalized_ == make_lower(other);
  }

  // 与 C 字符串比较
  bool operator==(const char* other) const {
    return (other != nullptr) && normalized_ == make_lower(other);
  }

  // 与 string_view 比较
  bool operator==(std::string_view other) const {
    return normalized_ == make_lower(other);
  }

  // ==== 哈希：直接哈希小写版本，O(1) ====
  size_t hash() const {
    return std::hash<std::string>()(normalized_);
  }

  // 显式转换
  operator std::string() const { return name_; }

  std::string to_string() const {
    return display_name();
  }
 private:
  // 全部小写转换
  static std::string make_lower(std::string_view s) {
    std::string result(s);
    for (char& c : result) {
      c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return result;
  }

  std::string name_;       // 原始形式（可能带引号去除后的内容）
  std::string normalized_; // 小写形式（用于比较/哈希）
  bool quoted_ = false;    // 原始是否带引号（通常为 false，因为解析器已去掉）
};

// ==== 全局便捷函数 ====

inline Identifier identifier(std::string_view name) {
  return Identifier(name);
}

inline std::string to_string(const Identifier& id) {
  return id.display_name();
}

// ==== 哈希特化 ====
struct IdentifierHash {
  size_t operator()(const Identifier& id) const {
    return id.hash();
  }
};

// ==== 字符串操作 ====
inline std::string operator+(const std::string& lhs, const Identifier& rhs) {
  return lhs + rhs.str();
}

inline std::string operator+(const Identifier& lhs, const std::string& rhs) {
  return lhs.str() + rhs;
}

// ==== 与字符串比较 ====
inline bool iequals(const Identifier& lhs, const std::string& rhs) {
  return lhs == rhs;
}

// ==== 打印 ====
inline std::ostream& operator<<(std::ostream& os, const Identifier& id) {
  os << id.display_name();
  return os;
}

}  // namespace sql

// std::hash 特化
namespace std {
template <>
struct hash<sql::Identifier> {
  size_t operator()(const sql::Identifier& id) const {
    return id.hash();
  }
};
}  // namespace std

template <>
struct fmt::formatter<sql::Identifier> : fmt::formatter<std::string> {
    // 如果 Identifier 有 to_string() 方法
    auto format(const sql::Identifier& id, format_context& ctx) const {
        return fmt::formatter<std::string>::format(id.to_string(), ctx);
    }
    
    // 如果 Identifier 内部存储的是 std::string name_ 成员，且没有 to_string()
    // 你可以直接访问它的公开成员或方法：
    // auto format(const sql::Identifier& id, format_context& ctx) const {
    //     return fmt::formatter<std::string>::format(id.name(), ctx);
    // }
};
