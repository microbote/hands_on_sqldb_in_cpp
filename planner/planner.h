// planner.h
//
// Planner：OptimizedQuery -> Plan 树（规则驱动，没有代价模型）。
//
// 规则（见 readme：这一层只做 ScanPlan 的规划）：
//   - SELECT：候选空间被主键收窄（ranges/keys）-> IndexScanPlan，
//     否则 FullScan。
//   - INSERT/UPDATE/DELETE：各自的写计划；UPDATE/DELETE 没有 WHERE 时
//     退化成全表扫描（**不能**当成"零行"）。
//   - DDL / USE：一一对应的计划节点，不涉及扫描。
#pragma once

#include <expected>
#include <memory>

#include "optimizer.h"
#include "plan.h"

namespace plan {

class Planner {
public:
  Planner() = default;
  virtual ~Planner() = default;

  // 返回堆上的计划节点：PlanNode 是多态基类，按值返回必然切片
  // （原接口的 `std::expected<PlanNode, PlanError>` 无法编译）。
  std::expected<std::unique_ptr<PlanNode>, PlanError>
  plan(OptimizedQuery query);
};

} // namespace plan
