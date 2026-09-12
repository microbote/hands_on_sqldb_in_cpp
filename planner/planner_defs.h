#pragma once

#include <cstdint>
#include <string>

namespace plan {

// ============================================================
// 计划节点的输出顺序
//
// 扫描出来的行天然按主键 key 的字节序（= 主键升序）；需要降序时**不翻转
// 区间数组**，而是让游标反向迭代 —— 一个计划只有一种区间表示。
//
// Sort 消除的判据就是它：ORDER BY 的首列是主键时，扫描顺序已经满足要求
// （主键唯一，后面的排序键永远分不出胜负），不需要 Sort 节点。
// ============================================================
enum class Ordering : uint8_t {
  NONE = 0,  // 无序
  PK_ASC,    // 主键升序
  PK_DESC,   // 主键降序（反向扫描）
};

inline const char *ordering_to_string(Ordering ordering) {
  switch (ordering) {
  case Ordering::PK_ASC:
    return "PK_ASC";
  case Ordering::PK_DESC:
    return "PK_DESC";
  default:
    return "NONE";
  }
}

// 与 statement 模块的 StmtError 同构：错误码用于控制流，message 用于日志/CLI
enum class PlanErrorCode : uint8_t {
  OK = 0,

  // ---- rewriter ----
  UNSUPPORTED_QUERY = 1, // 该语句类型不参与重写
  INVALID_CONDITION,     // 条件树结构异常

  // ---- optimizer ----
  CATALOG_NOT_OPEN, // 没有选中数据库
  TABLE_NOT_FOUND,
  COLUMN_NOT_FOUND,
  NO_PRIMARY_KEY, // 表没有主键：只能全表扫描（不一定是错误）
  EMPTY_TABLE_NAME,

  // ---- planner ----
  UNSUPPORTED_PLAN, // 无法为该 Query 生成计划
};

inline const char *plan_error_message(PlanErrorCode code) {
  switch (code) {
  case PlanErrorCode::OK:
    return "OK";
  case PlanErrorCode::UNSUPPORTED_QUERY:
    return "Unsupported query for rewriting";
  case PlanErrorCode::INVALID_CONDITION:
    return "Invalid condition tree";
  case PlanErrorCode::CATALOG_NOT_OPEN:
    return "Catalog is not open";
  case PlanErrorCode::TABLE_NOT_FOUND:
    return "Table not found";
  case PlanErrorCode::COLUMN_NOT_FOUND:
    return "Column not found";
  case PlanErrorCode::NO_PRIMARY_KEY:
    return "Table has no primary key";
  case PlanErrorCode::EMPTY_TABLE_NAME:
    return "Empty table name";
  case PlanErrorCode::UNSUPPORTED_PLAN:
    return "Unsupported plan";
  default:
    return "Unknown error";
  }
}

struct PlanError {
  PlanErrorCode code = PlanErrorCode::OK;
  std::string message;

  PlanError() = default;
  PlanError(PlanErrorCode c, std::string msg)
      : code(c), message(std::move(msg)) {}

  bool ok() const { return code == PlanErrorCode::OK; }
  explicit operator bool() const { return ok(); }

  std::string to_string() const {
    return message.empty() ? std::string(plan_error_message(code)) : message;
  }
};

} // namespace plan
