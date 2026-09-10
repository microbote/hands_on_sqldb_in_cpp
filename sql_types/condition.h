// condition.h
#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

namespace sql {

enum class ConditionType : uint8_t {
  COMPARE,  // 单值比较
  IN,       // IN / NOT IN
  AND,      // 逻辑与
  OR,       // 逻辑或
  NOT       // 逻辑非
};

inline const char* condition_type_to_string(ConditionType type) {
  switch (type) {
    case ConditionType::COMPARE: return "COMPARE";
    case ConditionType::IN:      return "IN";
    case ConditionType::AND:     return "AND";
    case ConditionType::OR:      return "OR";
    case ConditionType::NOT:     return "NOT";
  }
  return "UNKNOWN";
}

class ConditionVisitor;

class Condition {
 public:
  virtual ~Condition() = default;

  // 类型判断
  virtual ConditionType type() const = 0;
  
  bool is_leaf() const {
    return type() == ConditionType::COMPARE || type() == ConditionType::IN;
  }
  bool is_internal() const { return !is_leaf(); }
  bool is_compare() const { return type() == ConditionType::COMPARE; }
  bool is_in() const { return type() == ConditionType::IN; }
  bool is_and() const { return type() == ConditionType::AND; }
  bool is_or() const { return type() == ConditionType::OR; }
  bool is_not() const { return type() == ConditionType::NOT; }

  // 子节点访问（叶子返回空，内部节点返回子节点列表）
  // 返回子节点的数量（叶子和 NOT 为 0/1，AND/OR 为 2）
  virtual size_t child_count() const = 0;
  virtual const Condition* child_at(size_t index) const = 0;
  
  // clone
  virtual std::unique_ptr<Condition> clone() const = 0;

  // Visitor 模式
  virtual void accept(ConditionVisitor& visitor) const = 0;

  // 调试
  virtual std::string to_string() const = 0;
};

using ConditionPtr = std::unique_ptr<Condition>;

}  // namespace sql
