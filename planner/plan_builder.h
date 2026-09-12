#pragma once

#include "plan.h"
#include "query/statement/condition.h"
#include "query/statement/scan_converter.h"
#include "relation/sql_relation.h"

/*

SQL: SELECT id, name FROM users WHERE id > 10 AND id < 20 AND age > 18

1. Parser → AST
2. StatementBuilder → SelectStatement
3. extract_primary_key_conditions()
   ├── pk_cond: id > 10 AND id < 20
   └── remaining: age > 18

4. build_plan_from_extract_result()
   ├── IndexScanPlan: id > 10 AND id < 20
   │   ├── key_range: [10, 20)
   │   └── columns: [id, name]
   └── FilterPlan: age > 18
       └── child: IndexScanPlan

5. Executor 执行
   ├── ProjectExecutor → FilterExecutor → IndexScanExecutor
   │   ├── IndexScanExecutor: 扫描 (10, 20) 返回 id, name
   │   ├── FilterExecutor: 过滤 age > 18
   │   └── ProjectExecutor: 投影 id, name
   └── 逐行返回结果

*/
namespace query {

  // ============================================================
  // 从 ConditionExtractResult 构建执行计划
  // ============================================================
  std::unique_ptr<PlanNode> build_plan_from_extract_result(
      const ConditionExtractResult& result,
      const std::string& table_name,
      const std::vector<std::string>& columns,
      const sql::TableSchema& schema) {
      
      std::unique_ptr<PlanNode> root = nullptr;
      
      // 1. 构建 IndexScan（pk_cond）
      if (result.pk_cond) {
          auto desc = convert_to_scan_descriptor(result.pk_cond.get());
          auto index_plan = std::make_unique<IndexScanPlan>();
          index_plan->table_name = table_name;
          index_plan->columns = columns;
          
          // 填充扫描描述
          if (desc.has_points()) {
              index_plan->is_point_query = true;
              for (const auto& v : desc.point_set) {
                  index_plan->key_set.push_back(v.to_string());
              }
          } else if (desc.has_ranges()) {
              index_plan->is_point_query = false;
              index_plan->key_range = desc.range_set[0];  // 取第一个范围
          }
          
          // 排除集
          for (const auto& v : desc.excluded) {
              index_plan->excluded_keys.push_back(v.to_string());
          }
          
          root = std::move(index_plan);
      } else {
          // 没有主键条件，全表扫描
          // 可以创建一个 FullScanPlan 或返回 nullptr
          return nullptr;
      }
      
      // 2. 添加 Filter（remaining）
      if (result.remaining) {
          auto filter_plan = std::make_unique<FilterPlan>();
          filter_plan->child = std::move(root);
          filter_plan->condition = std::move(result.remaining);
          root = std::move(filter_plan);
      }
      
      // 3. 添加 Project（如果需要）
      if (!columns.empty() && !(columns.size() == 1 && columns[0] == "*")) {
          auto project_plan = std::make_unique<ProjectPlan>();
          project_plan->child = std::move(root);
          project_plan->columns = columns;
          root = std::move(project_plan);
      }
      
      return root;
  }
  
  } // namespace query