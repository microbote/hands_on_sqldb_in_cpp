// query_clause.h
#pragma once

#include <cstddef>
#include <optional>
#include <string>
#include <vector>

namespace sql {

// ============================================================
// OrderBy 排序方向
// ============================================================
enum class OrderDirection {
  ASC,
  DESC,
};

// ============================================================
// OrderBy 项
// ============================================================
struct OrderByItem {
  std::string column;
  OrderDirection direction = OrderDirection::ASC;

  // 构造函数
  OrderByItem() = default;
  OrderByItem(std::string col, OrderDirection dir = OrderDirection::ASC)
      : column(std::move(col)), direction(dir) {}

  // 便捷创建
  static OrderByItem asc(std::string col) {
    return OrderByItem(std::move(col), OrderDirection::ASC);
  }

  static OrderByItem desc(std::string col) {
    return OrderByItem(std::move(col), OrderDirection::DESC);
  }
};

// 比较两个排序项（优化器判断排序顺序是否有用）
inline bool is_ascending(const OrderByItem& item) {
  return item.direction == OrderDirection::ASC;
}

// ============================================================
// Limit 子句
// ============================================================
struct LimitClause {
  // 是否有限制（SELECT 没有 LIMIT 时 limit 字段为空）
  std::optional<size_t> row_count;   // LIMIT n

  // 是否有偏移（OFFSET m 不一定要有 LIMIT）
  std::optional<size_t> offset;

  LimitClause() = default;

  // 便捷构造
  static LimitClause limit(size_t n) {
    LimitClause c;
    c.row_count = n;
    return c;
  }

  static LimitClause limit_offset(size_t n, size_t m) {
    LimitClause c;
    c.row_count = n;
    c.offset = m;
    return c;
  }

  static LimitClause offset_only(size_t m) {
    LimitClause c;
    c.offset = m;
    return c;
  }

  bool has_limit() const { return row_count.has_value(); }
  bool has_offset() const { return offset.has_value(); }

  size_t limit_value() const {
    return row_count.value_or(0);
  }

  size_t offset_value() const {
    return offset.value_or(0);
  }
};

}  // namespace sql