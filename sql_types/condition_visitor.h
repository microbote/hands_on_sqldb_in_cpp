// condition_visitor.h
#pragma once

#include <cstddef>

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
//
// 五个 visit 都是纯虚：漏写 override 会在编译期报错，
// 而不是静默什么都不做（旧的默认空实现很容易埋雷，
// 例如新增一个节点类型后旧 visitor 全部静默跳过）。
//
// 只关心部分节点的 visitor 请用下面的 ConditionVisitorBase：
// 它提供空实现，供内部工具类继承。
// ============================================================
class ConditionVisitor {
 public:
  virtual ~ConditionVisitor() = default;

  // ---- 叶子节点 ----
  virtual void visit(const CompareCondition&) = 0;
  virtual void visit(const InCondition&) = 0;

  // ---- 内部节点 ----
  virtual void visit(const AndCondition&) = 0;
  virtual void visit(const OrCondition&) = 0;
  virtual void visit(const NotCondition&) = 0;
};

// 便利基类：默认什么都不做，只 override 需要处理的节点
class ConditionVisitorBase : public ConditionVisitor {
 public:
  void visit(const CompareCondition&) override {}
  void visit(const InCondition&) override {}
  void visit(const AndCondition&) override {}
  void visit(const OrCondition&) override {}
  void visit(const NotCondition&) override {}
};

// ============================================================
// 便捷辅助函数
// ============================================================
// 对整个条件树执行 visitor（深度优先，先父后子）
inline void walk_condition_pre_order(const Condition& cond, ConditionVisitor& visitor) {
  cond.accept(visitor);
  for(size_t i=0; i < cond.child_count(); i++){
    walk_condition_pre_order(*cond.child_at(i), visitor);
  }
}

inline void walk_condition_post_order(const Condition& cond, ConditionVisitor& visitor) {
  for(size_t i=0; i < cond.child_count(); i++){
    walk_condition_post_order(*cond.child_at(i), visitor);
  }
  cond.accept(visitor);
}

}  // namespace sql
