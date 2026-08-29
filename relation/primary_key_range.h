// primary_key_range.h
#pragma once

#include <optional>

#include "value.h"

namespace sql {

// ============================================================
// 扫描方向
// ============================================================
enum class ScanDirection {
  kForward,  // 升序
  kReverse   // 降序
};

// ============================================================
// 主键范围（逻辑范围，SQL 层使用）
// ============================================================
struct PrimaryKeyRange {
  std::optional<Value> start;  // 起始主键（包含）
  std::optional<Value> end;    // 结束主键（不包含）
  ScanDirection direction = ScanDirection::kForward;
  size_t limit = 0;  // 0 表示不限制

  // ----- 工厂方法 -----
  static PrimaryKeyRange all() { return {}; }

  static PrimaryKeyRange range(const Value& start, const Value& end) {
    PrimaryKeyRange r;
    r.start = start;
    r.end = end;
    return r;
  }

  static PrimaryKeyRange from(const Value& start) {
    PrimaryKeyRange r;
    r.start = start;
    return r;
  }

  static PrimaryKeyRange to(const Value& end) {
    PrimaryKeyRange r;
    r.end = end;
    return r;
  }

  // ----- 查询 -----
  bool is_all() const { return !start && !end; }
  bool has_start() const { return start.has_value(); }
  bool has_end() const { return end.has_value(); }
  bool has_limit() const { return limit > 0; }

  // ----- 修改器（返回新对象） -----
  PrimaryKeyRange with_direction(ScanDirection dir) const {
    PrimaryKeyRange r = *this;
    r.direction = dir;
    return r;
  }

  PrimaryKeyRange reverse() const {
    PrimaryKeyRange r = *this;
    r.direction = (direction == ScanDirection::kForward)
                      ? ScanDirection::kReverse
                      : ScanDirection::kForward;
    return r;
  }

  PrimaryKeyRange with_limit(size_t lim) const {
    PrimaryKeyRange r = *this;
    r.limit = lim;
    return r;
  }

  // ----- 调试 -----
  std::string to_string() const {
    std::string s = "PrimaryKeyRange[";
    s += start ? "start=" + start->to_string() : "start=∞";
    s += ", ";
    s += end ? "end=" + end->to_string() : "end=∞";
    s += ", dir=" +
         std::string(direction == ScanDirection::kForward ? "fwd" : "rev");
    if (limit > 0) s += ", limit=" + std::to_string(limit);
    s += "]";
    return s;
  }
};

}  // namespace sql