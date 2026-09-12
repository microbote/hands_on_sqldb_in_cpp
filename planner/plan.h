// plan.h
#ifndef QUERY_PLAN_H
#define QUERY_PLAN_H

#include <memory>
#include <string>
#include <vector>

#include "relation/sql_relation.h"
#include "query/statement/condition.h"
#include "query/statement/scan_desc.h"

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
// IndexScanPlan：索引扫描计划
// ============================================================
class IndexScanPlan : public PlanNode {
  public:
      std::string table_name;
      std::vector<std::string> columns;        // 需要返回的列
      
      // ===== 扫描方式 =====
      bool is_point_query = false;             // true: 点查询, false: 范围查询
      
      // 点查询
      std::vector<std::string> key_set;        // 点查询的 key 列表
      
      // 范围查询
      KeyRange key_range;                      // 范围 [start, end)
      
      // 排除集（用于过滤）
      std::vector<std::string> excluded_keys;  // 需要排除的 key
      
      // ===== 排序和限制（已下推） =====
      bool order_by_pk = false;
      bool ascending = true;
      size_t limit = 0; 
      
      // ===== 执行状态（运行时） =====
      // 由执行器维护，不在 Plan 中存储
      
      PlanType type() const override { return PlanType::INDEX_SCAN; }
      
      std::string to_string() const override {
          std::string s = "IndexScan(" + table_name;
          if (is_point_query) {
              s += ", points: " + std::to_string(key_set.size()) + " keys";
          } else {
              s += ", range: " + key_range.to_string();
          }
          if (!excluded_keys.empty()) {
              s += ", excluded: " + std::to_string(excluded_keys.size()) + " keys";
          }
          if (order_by_pk) {
              s += ", order_by: " + std::string(ascending ? "ASC" : "DESC");
          }
          if (limit > 0) {
              s += ", limit: " + std::to_string(limit);
          }
          s += ")";
          return s;
      }
  };
  
  // ============================================================
  // FilterPlan：过滤计划
  // ============================================================
  class FilterPlan : public PlanNode {
  public:
      std::unique_ptr<PlanNode> child;         // 子节点
      std::unique_ptr<ConditionExpr> condition; // 过滤条件
      
      PlanType type() const override { return PlanType::FILTER; }
      
      std::string to_string() const override {
          std::string s = "Filter(";
          if (condition) {
              s += condition->to_string();
          }
          s += ")";
          if (child) {
              s += " -> " + child->to_string();
          }
          return s;
      }
  };
  
  // ============================================================
  // ProjectPlan：投影计划
  // ============================================================
  class ProjectPlan : public PlanNode {
  public:
      std::unique_ptr<PlanNode> child;
      std::vector<std::string> columns;        // 需要投影的列
      
      PlanType type() const override { return PlanType::PROJECT; }
      
      std::string to_string() const override {
          std::string s = "Project(";
          for (size_t i = 0; i < columns.size(); ++i) {
              if (i > 0) s += ", ";
              s += columns[i];
          }
          s += ")";
          if (child) {
              s += " -> " + child->to_string();
          }
          return s;
      }
  };

}  // namespace query

#endif  // QUERY_PLAN_H