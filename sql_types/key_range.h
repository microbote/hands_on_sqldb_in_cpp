// key_range.h
#pragma once

#include <optional>
#include <string>
#include <vector>

#include "value.h"

namespace sql {

// ============================================================
// 区间类型
// ============================================================
enum class RangeType : uint8_t {
  EMPTY,  // 空集
  ALL,    // 全集 (-∞, +∞)
  BOUNDED // 有界区间 [start, end)
};

// ============================================================
// KeyRange 表示 [start, end) 左闭右开区间
// 支持整数、字符串等类型的值
// ============================================================
class KeyRange {
private:
  RangeType type_;
  sql::Value start_;
  sql::Value end_;

public:
  // ============================================================
  // 构造
  // ============================================================
  KeyRange() : type_(RangeType::ALL) {}

  KeyRange(RangeType type) : type_(type) {}

  // 工厂方法
  static KeyRange empty() { return KeyRange(RangeType::EMPTY); }

  static KeyRange all() { return KeyRange(); }

  static KeyRange range(const sql::Value &start, const sql::Value &end) {
    KeyRange r;

    r.start_ = start;
    r.end_ = end;
    if (start.is_null() && end.is_null()) {
      r.type_ = RangeType::ALL;
    } else if (start >= end) {
      r.type_ = RangeType::EMPTY;
    } else {
      r.type_ = RangeType::BOUNDED;
    }

    return r;
  }

  static KeyRange from(const sql::Value &start) {
    KeyRange r;
    if (start.is_null()) {
      return empty();
    }
    r.type_ = RangeType::BOUNDED;
    r.start_ = start;
    return r;
  }

  static KeyRange to(const sql::Value &end) {
    KeyRange r;
    if (end.is_null()) {
      return empty();
    }
    r.type_ = RangeType::BOUNDED;
    r.end_ = end;

    return r;
  }

  // 从点构造精确区间 [p, p+1) 适用于整数类型
  static KeyRange point(const sql::Value &p) {
    if (p.is_int()) {
      return range(p, sql::Value(p.int_val() + 1));
    }
    // 非整数无法精确表示点，返回空
    return empty();
  }

  // ============================================================
  // 访问器
  // ============================================================
  RangeType type() const { return type_; }
  bool is_empty() const { return type_ == RangeType::EMPTY; }
  bool is_all() const { return type_ == RangeType::ALL; }
  bool is_bounded() const { return type_ == RangeType::BOUNDED; }

  const sql::Value &start() const { return start_; }
  const sql::Value &end() const { return end_; }

  bool has_start() const { return is_bounded() && !start_.is_null(); }
  bool has_end() const { return is_bounded() && !end_.is_null(); }

  // ============================================================
  // 查询
  // ============================================================

  // 结构合法性：区间定义是否自洽
  bool is_valid() const {
    if (is_empty() || is_all()) {
      return true;
    }
    if (is_bounded()) {
      return !(has_start() && has_end() && start_ >= end_);
    }
    return false;
  }

  // 非空：是否包含至少一个值
  bool is_nonempty() const {
    if (is_empty()) {
      return false;
    }
    if (is_all()) {
      return true;
    }
    if (is_bounded()) {
      return !(has_start() && has_end() && start_ >= end_);
    }
    return false;
  }

  // 是否为单点区间（仅对整数类型有效）
  bool is_point() const {
    if (!is_bounded() || !has_start() || !has_end()) {
      return false;
    }
    if (!start_.is_int() || !end_.is_int()) {
      return false;
    }
    return (end_.int_val() - start_.int_val()) == 1;
  }

  // 获取单点值（仅当 is_point() 为 true）
  sql::Value point_value() const {
    if (is_point()) {
      return start_;
    }
    return sql::Value();
  }

  // 估算区间元素个数（仅对整数类型有效）
  std::optional<int64_t> size() const {
    if (!is_bounded() || !has_start() || !has_end()) {
      return std::nullopt;
    }
    if (!start_.is_int() || !end_.is_int()) {
      return std::nullopt;
    }
    int64_t count = end_.int_val() - start_.int_val();
    if (count <= 0) {
      return std::nullopt;
    }
    return count;
  }

  // 检查是否包含某个值
  bool contains(const sql::Value &val) const {
    if (is_empty()) {
      return false;
    }
    if (is_all()) {
      return true;
    }
    if (is_bounded()) {
      if (has_start() && val < start_) {
        return false;
      }
      if (has_end() && val >= end_) {
        return false;
      }
      return true;
    }
    return false;
  }

  // 检查两个区间是否重叠（有公共元素）
  bool overlaps(const KeyRange &other) const {
    if (is_empty() || other.is_empty()) {
      return false;
    }
    if (is_all() || other.is_all()) {
      return true;
    }
    // 两个有界区间
    if (has_start() && other.has_end() && start_ >= other.end_) {
      return false;
    }
    if (has_end() && other.has_start() && end_ <= other.start_) {
      return false;
    }
    return true;
  }

  // 检查两个区间是否相邻（一个的 end 等于另一个的 start）
  bool adjacent(const KeyRange &other) const {
    if (is_empty() || other.is_empty()) {
      return false;
    }
    if (is_all() || other.is_all()) {
      return false;
    }
    if (has_end() && other.has_start() && end_ == other.start_) {
      return true;
    }
    if (has_start() && other.has_end() && start_ == other.end_) {
      return true;
    }
    return false;
  }

  // 检查是否完全覆盖另一个区间（包括相等）
  bool covers(const KeyRange &other) const {
    if (is_empty()) {
      return other.is_empty();
    }
    if (is_all()) {
      return true;
    }
    if (other.is_empty()) {
      return true;
    }
    if (other.is_all()) {
      return false;
    }
    // 两者都是 BOUNDED
    if (has_start() && !other.has_start()) {
      return false;
    }
    if (has_end() && !other.has_end()) {
      return false;
    }
    if (has_start() && other.has_start() && start_ > other.start_) {
      return false;
    }
    if (has_end() && other.has_end() && end_ < other.end_) {
      return false;
    }
    return true;
  }

  // 两个区间是否相等
  bool equals(const KeyRange &other) const {
    if (type_ != other.type_) {
      return false;
    }
    if (is_empty() || is_all()) {
      return true;
    }
    if (start_ != other.start_) {
      return false;
    }
    if (end_ != other.end_) {
      return false;
    }
    return true;
  }

  // ============================================================
  // 集合运算
  // ============================================================
  // 交集
  KeyRange intersect(const KeyRange &other) const {
    if (is_empty() || other.is_empty()) {
      return empty();
    }
    if (is_all()) {
      return other;
    }
    if (other.is_all()) {
      return *this;
    }

    // 两个有界区间
    KeyRange result;
    result.type_ = RangeType::BOUNDED;

    // 取较晚的 start
    if (has_start() && other.has_start()) {
      result.start_ = (start_ > other.start_) ? start_ : other.start_;
    } else if (has_start()) {
      result.start_ = start_;
    } else if (other.has_start()) {
      result.start_ = other.start_;
    }

    // 取较早的 end
    if (has_end() && other.has_end()) {
      result.end_ = (end_ < other.end_) ? end_ : other.end_;
    } else if (has_end()) {
      result.end_ = end_;
    } else if (other.has_end()) {
      result.end_ = other.end_;
    }

    // 检查是否为空
    if (result.has_start() && result.has_end() &&
        result.start_ >= result.end_) {
      return empty();
    }
    return result;
  }

  // 并集（仅当两个区间重叠或相邻时才能合并）
  KeyRange unite(const KeyRange &other) const {
    if (is_empty()) {
      return other;
    }
    if (other.is_empty()) {
      return *this;
    }
    if (is_all() || other.is_all()) {
      return all();
    }

    // 检查是否重叠或相邻
    if (!overlaps(other) && !adjacent(other)) {
      // 不能合并，返回空（表示无法合并）
      return empty();
    }

    KeyRange result;
    result.type_ = RangeType::BOUNDED;

    // 取较早的 start
    if (has_start() && other.has_start()) {
      result.start_ = (start_ < other.start_) ? start_ : other.start_;
    }

    // 取较晚的 end
    if (has_end() && other.has_end()) {
      result.end_ = (end_ > other.end_) ? end_ : other.end_;
    }

    if (!result.has_start() && !result.has_end()) {
      return all();
    }

    return result;
  }

  // 差集: *this - other
  std::vector<KeyRange> subtract(const KeyRange &other) const {
    if (is_empty()) {
      return {empty()};
    }
    if (other.is_all()) {
      return {empty()};
    }
    if (other.is_empty()) {
      return {*this};
    }
    if (is_all()) {
      return to_complement(other);
    }
    // 有界区间 - 其他区间
    // 如果其他区间完全包含当前区间，返回空
    if (other.covers(*this)) {
      return {empty()};
    }
    // 如果其他区间与当前区间没有重叠，返回当前
    if (!overlaps(other)) {
      return {*this};
    }
    // 其他区间在当前区间内部或部分重叠
    // 如果 other 从左边开始
    if (!other.has_start() || other.start_ <= start_) {
      // 返回 [other.end, end)
      return {range(other.end_, end_)};
    }

    if (!other.has_end() || (has_end() && other.end_ >= end_)) {
      // 返回 [start, other.start)
      return {range(start_, other.start_)};
    }
    // other 在当前区间中间，产生两个区间
    return {range(start_, other.start_), range(other.end_, end_)};
  }

  std::vector<KeyRange> complement() const { return to_complement(*this); }

  // 补集（仅对全集或有限区间有效，可能产生两个区间）
  static auto to_complement(const KeyRange &r) -> std::vector<KeyRange> {
    if (r.is_empty()) {
      return {all()};
    }
    if (r.is_all()) {
      return {empty()};
    }
    std::vector<KeyRange> result;
    if (r.is_bounded()) {
      // (-∞, start) 和 (end, +∞)
      KeyRange left = to(r.start_);
      KeyRange right = from(r.end_);
      if (left.is_nonempty()) {
        result.push_back(left);
      }
      if (right.is_nonempty()) {
        result.push_back(right);
      }
    }
    return result;
  }

  // ============================================================
  // 序列化
  // ============================================================
  std::string to_string() const {
    if (is_empty()) {
      return "∅";
    }
    if (is_all()) {
      return "(-∞, +∞)";
    }
    std::string s = "[";
    s += has_start() ? start_.to_string() : "-∞";
    s += ", ";
    s += has_end() ? end_.to_string() : "+∞";
    s += ")";
    return s;
  }

  // ============================================================
  // 比较操作符
  // ============================================================
  bool operator==(const KeyRange &other) const { return equals(other); }
  bool operator!=(const KeyRange &other) const { return !equals(other); }
};

} // namespace sql