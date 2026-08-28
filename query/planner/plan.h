// plan.h
#ifndef QUERY_PLAN_H
#define QUERY_PLAN_H

#include <memory>
#include <string>
#include <vector>

#include "relation/sql_relation.h"
#include "query/statement/condition.h"

namespace query {

// ============================================================
// 计划节点类型
// ============================================================
enum class PlanType {
  SEQUENTIAL_SCAN,  // 全表扫描
  INDEX_SCAN,       // 索引扫描（主键范围）
  INSERT,           // 插入
  UPDATE,           // 更新
  DELETE,           // 删除
  FILTER,           // 过滤
  PROJECT           // 投影
};

// ============================================================
// 执行计划基类
// ============================================================
class PlanNode {
 public:
  virtual ~PlanNode() = default;
  virtual PlanType type() const = 0;
  virtual std::string to_string() const = 0;
};

// ============================================================
// 顺序扫描计划
// ============================================================
class SequentialScanPlan : public PlanNode {
 public:
  std::string table_name;
  std::unique_ptr<ConditionExpr> filter_condition;  // 应用层过滤
  std::vector<std::string> columns;                 // 需要返回的列

  SequentialScanPlan(const std::string& table) : table_name(table) {}
  PlanType type() const override { return PlanType::SEQUENTIAL_SCAN; }
  std::string to_string() const override {
    std::string result = "SeqScan(" + table_name + ")";
    if (filter_condition) {
      result += " [filter: " + filter_condition->to_string() + "]";
    }
    return result;
  }
};

// ============================================================
// 索引扫描计划（主键优化）
// ============================================================
class IndexScanPlan : public PlanNode {
 public:
  std::string table_name;
  std::vector<std::string> columns;
  bool is_point_query = false;  // true: 点查询, false: 范围查询
  sql::Value start_key;         // 范围起点（包含）
  sql::Value end_key;           // 范围终点（不包含）
  std::unique_ptr<ConditionExpr> filter_condition;  // 额外的非主键过滤

  IndexScanPlan(const std::string& table) : table_name(table) {}
  PlanType type() const override { return PlanType::INDEX_SCAN; }
  std::string to_string() const override {
    std::string result = "IndexScan(" + table_name + ", ";
    if (is_point_query) {
      result += "point: " + start_key.to_string();
    } else {
      result +=
          "range: [" + start_key.to_string() + ", " + end_key.to_string() + ")";
    }
    if (filter_condition) {
      result += ", filter: " + filter_condition->to_string();
    }
    result += ")";
    return result;
  }
};

// ============================================================
// 插入计划
// ============================================================
class InsertPlan : public PlanNode {
 public:
  std::string table_name;
  std::vector<std::string> columns;
  std::vector<sql::Value> values;

  InsertPlan(const std::string& table) : table_name(table) {}
  PlanType type() const override { return PlanType::INSERT; }
  std::string to_string() const override {
    return "Insert(" + table_name + ")";
  }
};

// ============================================================
// 更新计划
// ============================================================
class UpdatePlan : public PlanNode {
 public:
  std::string table_name;
  std::vector<std::pair<std::string, sql::Value>> assignments;
  std::unique_ptr<ConditionExpr> condition;

  UpdatePlan(const std::string& table) : table_name(table) {}
  PlanType type() const override { return PlanType::UPDATE; }
  std::string to_string() const override {
    return "Update(" + table_name + ")";
  }
};

// ============================================================
// 删除计划
// ============================================================
class DeletePlan : public PlanNode {
 public:
  std::string table_name;
  std::unique_ptr<ConditionExpr> condition;

  DeletePlan(const std::string& table) : table_name(table) {}
  PlanType type() const override { return PlanType::DELETE; }
  std::string to_string() const override {
    return "Delete(" + table_name + ")";
  }
};

// ============================================================
// 执行计划
// ============================================================
class ExecutionPlan {
 public:
  StatementType stmt_type;
  std::unique_ptr<PlanNode> root;
  std::string database_name;  // 执行时使用的数据库

  ExecutionPlan() : stmt_type(StatementType::UNKNOWN) {}
  explicit ExecutionPlan(StatementType type) : stmt_type(type) {}

  std::string to_string() const {
    if (root) {
      return root->to_string();
    }
    return "EmptyPlan";
  }
};

}  // namespace query

#endif  // QUERY_PLAN_H