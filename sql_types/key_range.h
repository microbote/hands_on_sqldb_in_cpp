// key_range.h
#pragma once

#include <optional>
#include <string>
#include <vector>

#include "value.h"

namespace sql {

class KeySet;
using Key = std::string;
struct StrKeyRange {
  Key low;
  Key high;
};

// ============================================================
// KeyRange: 左闭右开区间 [low, high)
// - 无独立状态存储，通过 low/high 自动推导语义
// - low = nullopt → -∞ (无下界)
// - high = nullopt → +∞ (无上界)
// - low && high && !(low < high) → 空集
// - !low && !high → 全集
// ============================================================
class KeyRange {
public:
  // ============================================================
  // 工厂方法
  // ============================================================

  // 全集 (-∞, +∞)
  // all() 无法推断类型，调用时需要指定
  static KeyRange all(DataType type = DataType::UNKNOWN_TYPE) {
    return KeyRange(type);
  }

  // 空集
  static KeyRange empty() {
    KeyRange r;
    r.low_ = Value(1);  // 任意哨兵值
    r.high_ = Value(0); // low > high → 自动推导为空集
    return r;
  }

  // 有界区间 [start, end)
  static KeyRange range(Value start, Value end) {
    KeyRange r;
    if (start.is_null() && end.is_null()) {
      return empty(); // 完全无法推断
    }
    if (!start.is_null()) {
      r.low_ = std::move(start);
      r.type_ = start.type();
    }
    if (!end.is_null()) {
      r.high_ = std::move(end);
      r.type_ = end.type();
    }
    return r;
  }

  // 左边界区间 [start, +∞)
  static KeyRange from(Value start) {
    KeyRange r;
    if (!start.is_null()) {
      r.low_ = std::move(start);
      r.type_ = start.type();
    }

    return r;
  }

  // 右边界区间 (-∞, end)
  static KeyRange to(Value end) {
    KeyRange r;
    if (!end.is_null()) {
      r.high_ = std::move(end);
      r.type_ = end.type();
    }
    return r;
  }

  // 单点区间（仅整数类型有意义，因为需要 [v, v+1)）
  static KeyRange point(const Value &v) {
    if (v.is_int()) {
      return range(v, Value(v.as_int() + 1));
    }
    return empty();
  }

  // 左闭右闭区间 [low, high]（内部转换为左闭右开）
  static KeyRange closed(Value low, Value high) {
    if (high.is_int()) {
      return range(std::move(low), Value(high.as_int() + 1));
    }
    return empty(); // 非整数无法精确表示闭区间
  }

  // ============================================================
  // 状态查询（通过边界推导）
  // ============================================================

  // 是否为空集
  bool is_empty() const {
    return low_ && high_ && !(*low_ < *high_); // low >= high
  }

  // 是否为全集 (-∞, +∞)
  bool is_all() const { return !low_ && !high_; }

  // 是否为有界非空区间 [a, b)
  bool is_bounded() const { return low_ && high_ && *low_ < *high_; }

  bool is_half() const { return !is_all() && (!low_ || !high_); }

  // 是否有下界
  bool has_low() const { return low_.has_value(); }

  // 是否有上界
  bool has_high() const { return high_.has_value(); }

  // 区间是否有效（非空集）
  bool is_valid() const { return !is_empty(); }

  // 区间是否非空且有实际元素
  bool is_nonempty() const { return !is_empty(); }

  // 是否为单点区间（整数且 [v, v+1)）
  bool is_point() const {
    if (!is_bounded())
      return false;
    if (!low_->is_int() || !high_->is_int())
      return false;
    return high_->as_int() - low_->as_int() == 1;
  }

  // 估算元素个数（仅整数有界区间）
  std::optional<int64_t> size() const {
    if (!is_bounded())
      return std::nullopt;
    if (!low_->is_int() || !high_->is_int())
      return std::nullopt;
    int64_t count = high_->as_int() - low_->as_int();
    if (count <= 0)
      return std::nullopt;
    return count;
  }
  // ============================================================
  // 类型推断
  // ============================================================

  DataType type() const { return type_; }

  bool has_known_type() const { return type_ != DataType::UNKNOWN_TYPE; }

  // 获取推断类型，无法推断时返回 false
  bool try_get_type(DataType &out) const {
    if (type_ != DataType::UNKNOWN_TYPE) {
      out = type_;
      return true;
    }
    return false;
  }

  // ============================================================
  // 序列化（自动处理类型推断）
  // ============================================================

  // 带默认类型参数，如果无法推断则使用默认
  StrKeyRange
  to_str_key_range(DataType fallback_type = DataType::UNKNOWN_TYPE) const {
    DataType effective_type = type_;
    if (effective_type == DataType::UNKNOWN_TYPE) {
      effective_type = fallback_type;
    }

    if (effective_type == DataType::UNKNOWN_TYPE) {
      // 仍然无法确定类型，返回空范围
      return {Value::min_key_for_type(DataType::UNKNOWN_TYPE),
              Value::upper_key_for_type(DataType::UNKNOWN_TYPE)};
    }

    return to_str_key_range_impl(effective_type);
  }

  // 兼容旧接口（必须知道类型）
  StrKeyRange to_str_key_range_with(DataType type) const {
    return to_str_key_range(type);
  }

  // ============================================================
  // 边界访问器
  // ============================================================

  const std::optional<Value> &low() const { return low_; }
  const std::optional<Value> &high() const { return high_; }

  // 边界值（调用前请确认 has_low() / has_high()）
  const Value &low_value() const { return *low_; }
  const Value &high_value() const { return *high_; }

  // 单点值（仅当 is_point() 为 true 时有效）
  Value point_value() const {
    if (is_point()) {
      return *low_;
    }
    return Value(); // null value
  }

  // ============================================================
  // 查询操作
  // ============================================================

  // 值是否在区间内
  bool contains(const Value &v) const {
    if (is_empty()) {
      return false;
    }
    if (v.is_null()) {
      return false;
    }

    if (low_ && v < *low_) {
      return false;
    }
    if (high_ && !(v < *high_)) {
      return false; // v >= high
    }

    return true;
  }

  // 两个区间是否重叠（有公共元素）
  bool overlaps(const KeyRange &other) const {
    if (is_empty() || other.is_empty()) {
      return false;
    }

    // 检查：this.low >= other.high → 不重叠
    if (low_ && other.high_ && !(*low_ < *other.high_)) {
      return false;
    }
    // 检查：other.low >= this.high → 不重叠
    if (other.low_ && high_ && !(*other.low_ < *high_)) {
      return false;
    }

    return true;
  }

  // 两个区间是否相邻（一个的 end == 另一个的 start）
  bool is_adjacent(const KeyRange &other) const {
    if (is_empty() || other.is_empty()) {
      return false;
    }

    if (high_ && other.low_ && *high_ == *other.low_) {
      return true;
    }
    if (low_ && other.high_ && *low_ == *other.high_) {
      return true;
    }

    return false;
  }

  // 当前区间是否完全覆盖 other
  bool covers(const KeyRange &other) const {
    if (is_empty()) {
      return other.is_empty();
    }
    if (other.is_empty()) {
      return true;
    }

    // 当前区间必须有更小（或相等）的下界
    if (low_) {
      if (!other.low_ || *low_ > *other.low_) {
        return false;
      }
    }
    // 当前区间必须有更大（或相等）的上界
    if (high_) {
      if (!other.high_ || *high_ < *other.high_) {
        return false;
      }
    }

    return true;
  }

  // 两个区间是否相等
  bool equals(const KeyRange &other) const {
    if (is_empty() && other.is_empty()) {
      return true;
    }
    if (is_empty() || other.is_empty()) {
      return false;
    }
    return low_ == other.low_ && high_ == other.high_;
  }

  // ============================================================
  // 集合运算
  // ============================================================

  // 交集
  KeyRange intersect(const KeyRange &other) const {
    if (is_empty() || other.is_empty()) {
      return empty();
    }

    // 下界：取较大者
    std::optional<Value> new_low;
    if (low_ && other.low_) {
      new_low = (*low_ > *other.low_) ? *low_ : *other.low_;
    } else if (low_) {
      new_low = *low_;
    } else if (other.low_) {
      new_low = *other.low_;
    }

    // 上界：取较小者
    std::optional<Value> new_high;
    if (high_ && other.high_) {
      new_high = (*high_ < *other.high_) ? *high_ : *other.high_;
    } else if (high_) {
      new_high = *high_;
    } else if (other.high_) {
      new_high = *other.high_;
    }

    // 构造结果（自动推导是否为空集）
    KeyRange result(type_);
    result.low_ = std::move(new_low);
    result.high_ = std::move(new_high);
    return result;
  }

  // 并集（仅当两个区间重叠或相邻时才能合并）
  std::vector<KeyRange> unite(const KeyRange &other) const {
    if (is_empty()) {
      return {other};
    }
    if (other.is_empty()) {
      return {*this};
    }

    // 不能合并的情况
    if (!overlaps(other) && !is_adjacent(other)) {
      return {*this, other};
    }

    // 下界：取较小者（如果一个无下界，结果无下界）
    std::optional<Value> new_low;
    if (low_ && other.low_) {
      new_low = (*low_ < *other.low_) ? *low_ : *other.low_;
    }

    // 上界：取较大者（如果一个无上界，结果无上界）
    std::optional<Value> new_high;
    if (high_ && other.high_) {
      new_high = (*high_ > *other.high_) ? *high_ : *other.high_;
    }

    KeyRange result(type_);
    result.low_ = std::move(new_low);
    result.high_ = std::move(new_high);
    return {result};
  }

  // 差集：*this - other（可能产生两个区间）
  std::vector<KeyRange> subtract(const KeyRange &other) const {
    if (is_empty()) {
      return {empty()};
    }
    if (other.is_empty()) {
      return {*this};
    }
    if (other.covers(*this)) {
      return {empty()};
    }
    if (!overlaps(other)) {
      return {*this};
    }

    std::vector<KeyRange> result;

    // 左部分: [this.low, other.low) 如果 this.low < other.low
    if (low_ && other.low_ && *low_ < *other.low_) {

        // 明确的有界左部分
        KeyRange left = range(*low_, *other.low_);

        result.push_back(left);
      }

    // 右部分: [other.high, this.high) 如果 other.high < this.high
    if (high_ && other.high_ && *other.high_ < *high_) {

        KeyRange right = range(*other.high_, *high_);

        result.push_back(right);
      }

    return result.empty() ? std::vector<KeyRange>{empty()} : result;
  }

  // 补集（可能产生两个区间）
  std::vector<KeyRange> complement() const {
    if (is_empty()) {
      return {all()};
    }
    if (is_all()) {
      return {empty()};
    }

    std::vector<KeyRange> result;

    // 左补集: (-∞, low)
    if (low_) {
      result.push_back(KeyRange::to(*low_));
    }

    // 右补集: [high, +∞)
    if (high_) {
      result.push_back(KeyRange::from(*high_));
    }

    return result;
  }

  // ============================================================
  // KeySet 交互方法（在 key_set.h 中实现）
  // ============================================================

  // 是否与 KeySet 有交集
  bool intersects_set(const KeySet &keys) const;

  // 从 KeySet 中过滤出在区间内的点
  KeySet filter_set(const KeySet &keys) const;

  // ============================================================
  // 序列化
  // ============================================================

  std::string to_string() const {
    if (is_empty()) {
      return "∅";
    }

    std::string s = "[";
    s += (low_) ? low_->to_string() : "-∞";
    s += ", ";
    s += (high_) ? high_->to_string() : "+∞";
    s += ")";
    return s;
  }

  // ============================================================
  // 比较操作符
  // ============================================================

  bool operator==(const KeyRange &other) const { return equals(other); }
  bool operator!=(const KeyRange &other) const { return !equals(other); }

private:
  // 私有默认构造 - 表示全集 (-∞, +∞)
  KeyRange() : type_(DataType::UNKNOWN_TYPE) {}

  KeyRange(DataType type) : type_(type) {}

  // 内部实现
  StrKeyRange to_str_key_range_impl(DataType type) const {
    if (is_empty()) {
      return {Value::upper_key_for_type(type), Value::min_key_for_type(type)};
    }

    StrKeyRange result;

    // 下界
    if (low_ && !low_->is_null()) {
      result.low = low_->to_key();
    } else {
      result.low = Value::min_key_for_type(type);
    }

    // 上界
    if (high_ && !high_->is_null()) {
      result.high = high_->to_key();
    } else {
      result.high = Value::upper_key_for_type(type);
    }

    return result;
  }

  // 边界值
  DataType type_ = DataType::UNKNOWN_TYPE;
  std::optional<Value> low_;  // nullopt = -∞
  std::optional<Value> high_; // nullopt = +∞
};

} // namespace sql