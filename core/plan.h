// plan.h
#ifndef PLAN_H
#define PLAN_H

#include <memory>
#include <string>
#include <vector>

#include "condition.h"
#include "storage_engine.h"

namespace sql {

// ============ 语句类型 ============
enum class StatementType { USE, SELECT, INSERT, UPDATE, DELETE, UNKNOWN };

// ============ 计划节点类型 ============
enum class PlanNodeType { SEQUENTIAL_SCAN, INDEX_SCAN, INSERT, UPDATE, DELETE };

// ============ 执行计划节点基类 ============
class PlanNode {
 public:
  virtual ~PlanNode() = default;
  virtual PlanNodeType type() const = 0;
  virtual std::string to_string() const = 0;
};

// ============ 顺序扫描计划 ============
class SequentialScanPlan : public PlanNode {
 public:
  std::string table_name;
  std::vector<std::string> columns;
  std::unique_ptr<ConditionExpr> filter_condition;  // 应用层过滤（非主键）

  SequentialScanPlan(const std::string& table) : table_name(table) {}

  PlanNodeType type() const override { return PlanNodeType::SEQUENTIAL_SCAN; }
  std::string to_string() const override {
    std::string result = "SequentialScan(" + table_name + ")";
    if (filter_condition) {
      result += " [filter: " + filter_condition->to_string() + "]";
    }
    return result;
  }
};

// ============ 索引扫描计划（主键优化） ============
class IndexScanPlan : public PlanNode {
 public:
  std::string table_name;
  std::vector<std::string> columns;
  storage::KeyRange key_range;  // 范围查询
  storage::KeySet key_set;      // 点查询
  bool is_point_query;          // true: 点查询, false: 范围查询
  std::unique_ptr<ConditionExpr> filter_condition;  // 额外的非主键过滤

  IndexScanPlan(const std::string& table)
      : table_name(table), is_point_query(false) {}

  PlanNodeType type() const override { return PlanNodeType::INDEX_SCAN; }

  std::string to_string() const override {
    std::string result = "IndexScan(" + table_name + ", ";
    if (is_point_query) {
      result += "points: " + std::to_string(key_set.size()) + " keys";
    } else {
      result += "range: [";
      result += key_range.has_start ? key_range.start : "";
      result += ", ";
      result += key_range.has_end ? key_range.end : "";
      result += ")";
    }
    if (filter_condition) {
      result += ", filter: " + filter_condition->to_string();
    }
    result += ")";
    return result;
  }
};

// ============ Insert 计划 ============
class InsertPlan : public PlanNode {
 public:
  std::string table_name;
  std::vector<std::string> columns;
  std::vector<storage::Value> values;

  InsertPlan(const std::string& table) : table_name(table) {}
  PlanNodeType type() const override { return PlanNodeType::INSERT; }
  std::string to_string() const override {
    return "Insert(" + table_name + ")";
  }
};

// ============ Update 计划 ============
class UpdatePlan : public PlanNode {
 public:
  std::string table_name;
  std::vector<std::pair<std::string, storage::Value>> assignments;
  std::unique_ptr<ConditionExpr> condition;  // 条件

  UpdatePlan(const std::string& table) : table_name(table) {}
  PlanNodeType type() const override { return PlanNodeType::UPDATE; }
  std::string to_string() const override {
    return "Update(" + table_name + ")";
  }
};

// ============ Delete 计划 ============
class DeletePlan : public PlanNode {
 public:
  std::string table_name;
  std::unique_ptr<ConditionExpr> condition;

  DeletePlan(const std::string& table) : table_name(table) {}
  PlanNodeType type() const override { return PlanNodeType::DELETE; }
  std::string to_string() const override {
    return "Delete(" + table_name + ")";
  }
};

// ============ 执行计划 ============
class ExecutionPlan {
 public:
  StatementType stmt_type;
  std::unique_ptr<PlanNode> root;

  ExecutionPlan() : stmt_type(StatementType::UNKNOWN) {}
  explicit ExecutionPlan(StatementType type) : stmt_type(type) {}

  std::string to_string() const {
    if (root) {
      return root->to_string();
    }
    return "EmptyPlan";
  }
};

}  // namespace sql

#endif  // PLAN_H