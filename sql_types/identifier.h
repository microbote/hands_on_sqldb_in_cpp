// identifier.h
#pragma once

#include <string>
#include <string_view>
#include <functional>
#include <utility>

namespace sql {

// ============================================================
// Identifier：数据库标识符（表名、列名、数据库名等）
// 不区分大小写存储和比较（SQL 标准语义）
// ============================================================
class Identifier {
 public:
  // ---- 构造 ----
  Identifier() = default;
  
  // 从字符串构造（保留原始大小写形式，但比较时不区分大小写）
  explicit Identifier(std::string name) : name_(std::move(name)) {}
  
  // 从 C 字符串构造
  explicit Identifier(const char* name) : name_(name ? name : "") {}
  
  // 从 string_view 构造
  explicit Identifier(std::string_view name) : name_(name) {}

  // ---- 访问器 ----
  const std::string& str() const { return name_; }
  const char* c_str() const { return name_.c_str(); }
  bool empty() const { return name_.empty(); }
  size_t size() const { return name_.size(); }

  // ---- 原始名称（保持大小写） ----
  const std::string& raw() const { return name_; }

  // ---- 规范化名称（转为小写，用于比较和哈希） ----
  std::string normalized() const {
    std::string result = name_;
    for (char& c : result) {
      c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return result;
  }

  // ---- 判断不带引号（保留大小写） ----
  // 如 "userName"（带引号） vs userName（不带引号）
  bool is_quoted() const { return quoted_; }
  void set_quoted(bool quoted) { quoted_ = quoted; }

  // 如果未加引号，一律小写存储（PostgreSQL 行为）
  // 如果加了引号，保持原始大小写
  static Identifier make(const std::string& name, bool quoted = false) {
    Identifier id;
    id.quoted_ = quoted;
    if (quoted) {
      id.name_ = name;  // 保持原样
    } else {
      id.name_ = to_lower(name);  // 小写化
    }
    return id;
  }

  // 用于 ORDER BY 等（展示原始名字）
  std::string display_name() const {
    if (quoted_) {
      return "\"" + name_ + "\"";
    }
    return name_;
  }

  // ---- 比较操作符（不区分大小写） ----
  bool operator==(const Identifier& other) const {
    return strcasecmp(name_.c_str(), other.name_.c_str()) == 0;
  }

  bool operator!=(const Identifier& other) const {
    return !(*this == other);
  }

  bool operator<(const Identifier& other) const {
    return normalized() < other.normalized();
  }

  // 与字符串比较
  bool operator==(const std::string& other) const {
    return strcasecmp(name_.c_str(), other.c_str()) == 0;
  }

  bool operator==(const char* other) const {
    return other && strcasecmp(name_.c_str(), other) == 0;
  }

  // ---- 隐式转换（谨慎使用，方便与旧代码集成） ----
  operator std::string() const { return name_; }
  
  // 显式转换
  explicit operator std::string_view() const { return name_; }

  // ---- 哈希支持（用于 unordered_map/set） ----
  size_t hash() const {
    return std::hash<std::string>()(normalized());
  }

 private:
  static std::string to_lower(std::string_view s) {
    std::string result(s);
    for (char& c : result) {
      c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return result;
  }

  std::string name_;     // 存储的名称（可能已小写化）
  bool quoted_ = false;  // 是否加引号（保留大小写）
};

// ============================================================
// 全局便捷函数
// ============================================================

inline Identifier identifier(const std::string& name, bool quoted = false) {
  return Identifier::make(name, quoted);
}

inline std::string to_string(const Identifier& id) {
  return id.display_name();
}

// ============================================================
// 哈希特化（用于 std::unordered_map/set）
// ============================================================
struct IdentifierHash {
  size_t operator()(const Identifier& id) const {
    return id.hash();
  }
};

// ============================================================
// 与 std::string 的无缝互操作
// ============================================================
inline std::string operator+(const std::string& lhs, const Identifier& rhs) {
  return lhs + rhs.str();
}

inline std::string operator+(const Identifier& lhs, const std::string& rhs) {
  return lhs.str() + rhs;
}

// ============================================================
// 字符串比较（比 operator== 更宽松）
// ============================================================
inline bool iequals(const Identifier& lhs, const std::string& rhs) {
  return lhs == rhs;
}

inline bool iequals(const Identifier& lhs, const char* rhs) {
  return lhs == rhs;
}

// ============================================================
// 打印（std::cout）
// ============================================================
inline std::ostream& operator<<(std::ostream& os, const Identifier& id) {
  os << id.display_name();
  return os;
}

}  // namespace sql

// ============================================================
// std::hash 特化（在 global namespace）
// ============================================================
namespace std {
template <>
struct hash<sql::Identifier> {
  size_t operator()(const sql::Identifier& id) const {
    return id.hash();
  }
};
}  // namespace std