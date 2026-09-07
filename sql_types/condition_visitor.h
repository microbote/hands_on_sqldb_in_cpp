// condition_visitor.h
#pragma once

#include "condition.h"

namespace sql {

// 前向声明具体类型（condition_types.h 中定义）
class CompareCondition;
class InCondition;
class AndCondition;
class OrCondition;
class NotCondition;

// ============================================================
// ConditionVisitor：所有 visitor 的基类
// 提供默认空实现，子类只需 override 感兴趣的方法
// ============================================================
class ConditionVisitor {
 public:
  virtual ~ConditionVisitor() = default;

  // ---- 叶子节点 ----
  virtual void visit(const CompareCondition&) {}
  virtual void visit(const InCondition&) {}

  // ---- 内部节点 ----
  virtual void visit(const AndCondition&) {}
  virtual void visit(const OrCondition&) {}
  virtual void visit(const NotCondition&) {}
};

// ============================================================
// 便捷辅助函数
// ============================================================
// 对整个条件树执行 visitor（深度优先，先父后子）
inline void walk_condition_pre_order(const Condition& cond, ConditionVisitor& visitor) {
  cond.accept(visitor);
  for(int i=0; i < cond.child_count(); i++){
    walk_condition_pre_order(*cond.child_at(i), visitor);
  }
}

inline void walk_condition_post_order(const Condition& cond, ConditionVisitor& visitor) {
  for(int i=0; i < cond.child_count(); i++){
    walk_condition_post_order(*cond.child_at(i), visitor);
  }
  cond.accept(visitor);
}

}  // namespace sql