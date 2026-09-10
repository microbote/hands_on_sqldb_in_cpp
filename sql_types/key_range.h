// key_range.h
#pragma once

#include <cstddef>
#include <optional>
#include <string>
#include <vector>

#include "key.h"
#include "value.h"

namespace sql {

class KeySet;

struct StrKeyRange {
  Key low;
  Key high;
};

// ============================================================
// KeyRange：对"编码后 key 空间"的区间
//
// 内部用 (Value, 含/不含标记) 表示边界，但所有判定都通过
// KeyCodecs 编码后的 key 字节序完成，因此：
//   - 区间语义与实际扫描使用的 key 空间完全一致（不会出现
//     contains() 说有、扫描却扫不到的情况）；
//   - 字符串也能精确表达单点/闭区间（key 编码是保序的）。
//
// 边界含义：
//   low_  == nullopt        → -∞
//   high_ == nullopt        → +∞
//   low_exclusive_  == true → 下界不含 low_，即 (low, ...
//   high_inclusive_ == true → 上界包含 high_，即 ..., high]
//   默认（两个 flag 均 false）→ [low, high)
// ============================================================
class KeyRange {
 public:
  // ============================================================
  // 工厂方法
  // ============================================================

  // 全集 (-∞, +∞)
  static KeyRange all(DataType type = DataType::UNKNOWN_TYPE) {
    return KeyRange(type);
  }

  // 空集
  static KeyRange empty(DataType type = DataType::UNKNOWN_TYPE) {
    KeyRange r(type);
    r.low_ = Value(1);   // 任意哨兵值
    r.high_ = Value(0);  // low > high → 自动推导为空集
    return r;
  }

  // 有界区间 [start, end)
  static KeyRange range(Value start, Value end) {
    KeyRange r;
    if (start.is_null() && end.is_null()) {
      return empty();  // 完全无法推断
    }
    if (!start.is_null()) {
      r.low_ = std::move(start);
      r.type_ = r.low_->type();
    }
    if (!end.is_null()) {
      r.high_ = std::move(end);
      r.type_ = r.high_->type();
    }
    return r;
  }

  // 左边界区间 [start, +∞)
  static KeyRange from(Value start) {
    KeyRange r;
    if (!start.is_null()) {
      r.low_ = std::move(start);
      r.type_ = r.low_->type();
    }
    return r;
  }

  // 右边界区间 (-∞, end)
  static KeyRange to(Value end) {
    KeyRange r;
    if (!end.is_null()) {
      r.high_ = std::move(end);
      r.type_ = r.high_->type();
    }
    return r;
  }

  // 单点区间 {v}
  //  - 整数：用 [v, v+1)（整数域上 +1 就是下一个候选值）
  //  - 其它类型：用 [v, v]（含上界），因为字符串/布尔没有"下一个值"
  static KeyRange point(const Value &v) {
    if (v.is_null()) {
      return empty();
    }
    if (v.is_int()) {
      return range(v, Value(v.as_int() + 1));
    }
    KeyRange r;
    r.low_ = v;
    r.high_ = v;
    r.high_inclusive_ = true;
    r.type_ = v.type();
    return r;
  }

  // 闭区间 [low, high]
  static KeyRange closed(Value low, Value high) {
    if (low.is_null() || high.is_null()) {
      return empty();
    }
    KeyRange r;
    r.low_ = std::move(low);
    r.high_ = std::move(high);
    r.high_inclusive_ = true;
    r.type_ = r.low_->type();
    return r;
  }

  // ============================================================
  // 边界（编码后的 key）—— 存储层适配用
  // ============================================================

  std::optional<Key> low_key() const {
    if (!low_) {
      return std::nullopt;
    }
    Key k = low_->to_key();
    if (k.empty()) {
      return std::nullopt;  // NULL 边界当作 -∞
    }
    if (low_exclusive_) {
      k = KeyCodecs::inclusive_upper_bound(k);
    }
    return k;
  }

  std::optional<Key> high_key() const {
    if (!high_) {
      return std::nullopt;
    }
    Key k = high_->to_key();
    if (k.empty()) {
      return std::nullopt;  // NULL 边界当作 +∞
    }
    if (high_inclusive_) {
      k = KeyCodecs::inclusive_upper_bound(k);
    }
    return k;
  }

  bool low_exclusive() const { return low_exclusive_; }
  bool high_inclusive() const { return high_inclusive_; }

  // ============================================================
  // 状态查询
  // ============================================================

  bool is_empty() const {
    const auto lk = low_key();
    const auto hk = high_key();
    return lk.has_value() && hk.has_value() && !(*lk < *hk);
  }

  bool is_all() const { return !low_ && !high_; }

  bool is_bounded() const {
    const auto lk = low_key();
    const auto hk = high_key();
    return lk.has_value() && hk.has_value() && *lk < *hk;
  }

  bool is_half() const { return !is_all() && (!low_ || !high_); }

  bool has_low() const { return low_.has_value(); }

  bool has_high() const { return high_.has_value(); }

  bool is_valid() const { return !is_empty(); }

  bool is_nonempty() const { return !is_empty(); }

  // 是否恰好包含一个值
  bool is_point() const {
    if (is_empty()) {
      return false;
    }
    if (!high_inclusive_ && !low_exclusive_ && low_ && high_ &&
        low_->is_int() && high_->is_int()) {
      return high_->as_int() - low_->as_int() == 1;
    }
    if (high_inclusive_ && !low_exclusive_ && low_ && high_) {
      return *low_ == *high_;
    }
    return false;
  }

  // 估算元素个数（仅整数有界区间）
  std::optional<int64_t> size() const {
    if (!low_ || !high_ || !low_->is_int() || !high_->is_int()) {
      return std::nullopt;
    }
    if (!is_bounded()) {
      return std::nullopt;
    }
    int64_t count = high_->as_int() - low_->as_int();
    if (high_inclusive_) {
      count += 1;
    }
    if (low_exclusive_) {
      count -= 1;
    }
    if (count <= 0) {
      return std::nullopt;
    }
    return count;
  }

  // ============================================================
  // 类型推断
  // ============================================================

  DataType type() const { return type_; }

  bool has_known_type() const { return type_ != DataType::UNKNOWN_TYPE; }

  bool try_get_type(DataType &out) const {
    if (type_ != DataType::UNKNOWN_TYPE) {
      out = type_;
      return true;
    }
    return false;
  }

  // ============================================================
  // 序列化
  // ============================================================

  StrKeyRange
  to_str_key_range(DataType fallback_type = DataType::UNKNOWN_TYPE) const {
    DataType effective_type = type_;
    if (effective_type == DataType::UNKNOWN_TYPE) {
      effective_type = fallback_type;
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
      return low_exclusive_ ? *high_ : *low_;
    }
    return Value();  // null value
  }

  // ============================================================
  // 查询操作（全部基于编码后 key 的字节序）
  // ============================================================

  bool contains(const Value &v) const {
    if (is_empty() || v.is_null()) {
      return false;
    }
    const Key k = v.to_key();
    if (k.empty()) {
      return false;  // NULL / UNKNOWN 没有 key
    }
    const auto lk = low_key();
    if (lk && k < *lk) {
      return false;
    }
    const auto hk = high_key();
    if (hk && !(k < *hk)) {
      return false;
    }
    return true;
  }

  // 两个区间是否重叠（有公共元素）
  bool overlaps(const KeyRange &other) const {
    if (is_empty() || other.is_empty()) {
      return false;
    }
    const auto lk = low_key();
    const auto hk = high_key();
    const auto olk = other.low_key();
    const auto ohk = other.high_key();

    if (lk && ohk && !(*lk < *ohk)) {
      return false;
    }
    if (olk && hk && !(*olk < *hk)) {
      return false;
    }
    return true;
  }

  // 两个区间是否相邻（一个的上界正好是另一个的下界）
  bool is_adjacent(const KeyRange &other) const {
    if (is_empty() || other.is_empty()) {
      return false;
    }
    const auto lk = low_key();
    const auto hk = high_key();
    const auto olk = other.low_key();
    const auto ohk = other.high_key();

    if (hk && olk && *hk == *olk) {
      return true;
    }
    if (lk && ohk && *lk == *ohk) {
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
    const auto lk = low_key();
    const auto hk = high_key();
    const auto olk = other.low_key();
    const auto ohk = other.high_key();

    if (lk && (!olk || *olk < *lk)) {
      return false;
    }
    if (hk && (!ohk || *hk < *ohk)) {
      return false;
    }
    return true;
  }

  bool equals(const KeyRange &other) const {
    if (is_empty() && other.is_empty()) {
      return true;
    }
    if (is_empty() || other.is_empty()) {
      return false;
    }
    return low_key() == other.low_key() && high_key() == other.high_key();
  }

  // ============================================================
  // 集合运算
  // ============================================================

  KeyRange intersect(const KeyRange &other) const {
    if (is_empty() || other.is_empty()) {
      return empty();
    }

    KeyRange result(merge_type(type_, other.type_));
    const auto lk = low_key();
    const auto olk = other.low_key();
    const auto hk = high_key();
    const auto ohk = other.high_key();

    // 下界：取较大者
    if (!lk) {
      result.low_ = other.low_;
      result.low_exclusive_ = other.low_exclusive_;
    } else if (!olk) {
      result.low_ = low_;
      result.low_exclusive_ = low_exclusive_;
    } else if (*lk < *olk) {
      result.low_ = other.low_;
      result.low_exclusive_ = other.low_exclusive_;
    } else {
      result.low_ = low_;
      result.low_exclusive_ = low_exclusive_;
    }

    // 上界：取较小者
    if (!hk) {
      result.high_ = other.high_;
      result.high_inclusive_ = other.high_inclusive_;
    } else if (!ohk) {
      result.high_ = high_;
      result.high_inclusive_ = high_inclusive_;
    } else if (*ohk < *hk) {
      result.high_ = other.high_;
      result.high_inclusive_ = other.high_inclusive_;
    } else {
      result.high_ = high_;
      result.high_inclusive_ = high_inclusive_;
    }

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
    if (!overlaps(other) && !is_adjacent(other)) {
      return {*this, other};
    }

    KeyRange result(merge_type(type_, other.type_));
    const auto lk = low_key();
    const auto olk = other.low_key();
    const auto hk = high_key();
    const auto ohk = other.high_key();

    // 下界：取较小者
    if (!lk) {
      result.low_ = low_;
      result.low_exclusive_ = low_exclusive_;
    } else if (!olk) {
      result.low_ = other.low_;
      result.low_exclusive_ = other.low_exclusive_;
    } else if (*lk < *olk) {
      result.low_ = low_;
      result.low_exclusive_ = low_exclusive_;
    } else {
      result.low_ = other.low_;
      result.low_exclusive_ = other.low_exclusive_;
    }

    // 上界：取较大者
    if (!hk) {
      result.high_ = high_;
      result.high_inclusive_ = high_inclusive_;
    } else if (!ohk) {
      result.high_ = other.high_;
      result.high_inclusive_ = other.high_inclusive_;
    } else if (*hk < *ohk) {
      result.high_ = other.high_;
      result.high_inclusive_ = other.high_inclusive_;
    } else {
      result.high_ = high_;
      result.high_inclusive_ = high_inclusive_;
    }

    return {result};
  }

  // 差集：*this - other（可能产生两个区间）
  std::vector<KeyRange> subtract(const KeyRange &other) const {
    if (is_empty()) {
      return {empty()};
    }
    if (other.is_empty() || !overlaps(other)) {
      return {*this};
    }
    if (other.covers(*this)) {
      return {empty()};
    }

    std::vector<KeyRange> result;

    // 左部分：[this.low, other.low)
    //   other 含自己的下界 -> 左部分不含 other.low
    //   other 不含自己的下界 -> 左部分可以含 other.low
    if (other.low_) {
      KeyRange left(merge_type(type_, other.type_));
      left.low_ = low_;
      left.low_exclusive_ = low_exclusive_;
      left.high_ = other.low_;
      left.high_inclusive_ = other.low_exclusive_;
      if (!left.is_empty() && !left.is_all()) {
        result.push_back(left);
      }
    }

    // 右部分：[other.high, this.high)
    //   other 含自己的上界 -> 右部分从 other.high 之后开始
    if (other.high_) {
      KeyRange right(merge_type(type_, other.type_));
      right.low_ = other.high_;
      right.low_exclusive_ = other.high_inclusive_;
      right.high_ = high_;
      right.high_inclusive_ = high_inclusive_;
      if (!right.is_empty() && !right.is_all()) {
        result.push_back(right);
      }
    }

    return result.empty() ? std::vector<KeyRange>{empty()} : result;
  }

  // 补集（可能产生两个区间）
  std::vector<KeyRange> complement() const {
    if (is_empty()) {
      return {all(type_)};
    }
    if (is_all()) {
      return {empty()};
    }

    std::vector<KeyRange> result;

    // 左补集：x < low 的部分
    if (low_) {
      KeyRange left(type_);
      left.high_ = low_;
      left.high_inclusive_ = low_exclusive_;
      if (!left.is_empty()) {
        result.push_back(left);
      }
    }

    // 右补集：x > high 的部分
    if (high_) {
      KeyRange right(type_);
      right.low_ = high_;
      right.low_exclusive_ = high_inclusive_;
      if (!right.is_empty()) {
        result.push_back(right);
      }
    }

    return result;
  }

  // ============================================================
  // KeySet 交互方法（在 key_range.cpp 中实现）
  // ============================================================

  bool intersects_set(const KeySet &keys) const;

  KeySet filter_set(const KeySet &keys) const;

  // ============================================================
  // 调试
  // ============================================================

  std::string to_string() const {
    if (is_empty()) {
      return "∅";
    }
    if (is_all()) {
      return "(-∞, +∞)";
    }
    std::string s = low_exclusive_ ? "(" : "[";
    s += low_ ? low_->to_string() : "-∞";
    s += ", ";
    s += high_ ? high_->to_string() : "+∞";
    s += high_inclusive_ ? "]" : ")";
    return s;
  }

  // ============================================================
  // 比较操作符
  // ============================================================

  bool operator==(const KeyRange &other) const { return equals(other); }

  bool operator!=(const KeyRange &other) const { return !equals(other); }

 private:
  // 私有构造 - 表示全集 (-∞, +∞)
  explicit KeyRange(DataType type = DataType::UNKNOWN_TYPE) : type_(type) {}

  // 类型合并：优先保留已知类型
  static DataType merge_type(DataType a, DataType b) {
    if (a != DataType::UNKNOWN_TYPE) {
      return a;
    }
    return b;
  }

  StrKeyRange to_str_key_range_impl(DataType type) const {
    if (is_empty()) {
      return {Value::upper_key_for_type(type), Value::min_key_for_type(type)};
    }

    StrKeyRange result;
    const auto lk = low_key();
    const auto hk = high_key();
    result.low = lk ? *lk : Value::min_key_for_type(type);
    result.high = hk ? *hk : Value::upper_key_for_type(type);
    return result;
  }

  DataType type_ = DataType::UNKNOWN_TYPE;
  std::optional<Value> low_;   // nullopt = -∞
  std::optional<Value> high_;  // nullopt = +∞
  bool low_exclusive_ = false;   // 下界不含 low_
  bool high_inclusive_ = false;  // 上界包含 high_
};

}  // namespace sql
