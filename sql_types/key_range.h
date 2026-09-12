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

// NULL 在范围里的位置（优化器按谓词语义选择）
enum class NullScope : uint8_t {
  INCLUDE, // 含 NULL（下界从族最小值开始）
  EXCLUDE, // 不含 NULL（下界从第一个非 NULL 值开始）
  ONLY,    // 只有 NULL
};

// ============================================================
// StrKeyRange：交给物理扫描的**半开区间** [start, end)
//
// 这是"边界压平"后的形式：语义层（KeyRange）的 NULL/开闭/inclusivity 都在
// to_str_key_range() 里被折叠进 key，物理层只需要一种形式 ——
// 这也是最初的设计意图 （扫描层不处理 corner case，`Seek(start)` + `key() <
// end` 即可）。
//
// 边界全部是具体 key：
//   start = -∞  → [tag][0x00]（NULL 的 key，也是族最小值）
//   end   = +∞  → [tag+1]    （恰好大于本族所有 key，且不被任何值产生）
//   含上界 v    → key(v) + 0x00（普适后继；不会溢出、不越族、不是合法编码）
//
// 注意：这是**扫描边界**，不是可以写进存储的合法 key——
// key()+0x00 与族上界都不满足 KeyCodecs::is_valid_key()。
// ============================================================
struct StrKeyRange {
  Key start;
  Key end;

  // 半开区间：start >= end 即空（|[x,x)| = ∅，倒置的也是空）
  bool is_empty() const { return !(start < end); }
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
//
// NULL 是一等边界（供优化器按谓词语义选择）：
//   low_ = NULL, low_exclusive_ = false  → 从族最小值（NULL 的 key）开始：**含
//   NULL** low_ = NULL, low_exclusive_ = true   → 从第一个非 NULL
//   值开始：**不含 NULL** high_ = NULL, high_inclusive_ = true → 上界就是
//   NULL：**只有 NULL**（单点）
//
//   便捷工厂：all() / non_null() / null_only() / gt() / ge() / lt() / le() /
//   eq() 查询接口：null_scope() / includes_null() / contains(Value())
//
//   注意：下界是"含 NULL"时等价于"无下界"（都从族最小值开始），
//   构造与集合运算结果都会 normalize() 成前者，避免同一区间有两种结构表示。
//   有具体值下界的区间（如 range(1, 10)）天然不含 NULL（NULL 排在最前）。
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

  // 只包含非 NULL 值的区间（下界 = "第一个非 NULL 值"）
  // 用法：优化器对 `IS NOT NULL` / 或判定"NULL 不可能满足该谓词"时选它。
  static KeyRange non_null(DataType type = DataType::UNKNOWN_TYPE) {
    KeyRange r(type);
    r.low_ = Value();        // NULL 作为下界
    r.low_exclusive_ = true; // 排除 NULL 本身
    return r;
  }

  // 只包含 NULL 的区间（单点）
  static KeyRange null_only(DataType type = DataType::UNKNOWN_TYPE) {
    KeyRange r(type);
    r.high_ = Value();
    r.high_inclusive_ = true;
    r.normalize(); // (-∞, NULL] 就是"只有 NULL"（下界等价于族最小值）
    return r;
  }

  // ============================================================
  // 比较谓词对应的范围（优化器用）
  //
  // NULL 语义按 SQL 三值逻辑：`id > 5`、`id <= 5` 这类比较**永远不匹配 NULL**，
  // 所以这些工厂默认把 NULL 排除掉（下界从"第一个非 NULL 值"开始），
  // 避免"物理上扫到 NULL、上层再过滤"的浪费。
  //   - 需要含 NULL 的全表扫描 → all()
  //   - IS NULL                    → null_only()
  //   - 参数为 NULL（比较永不成立）→ 空集
  // ============================================================

  // (v, +∞)
  static KeyRange gt(Value v) {
    if (v.is_null()) {
      return empty();
    }
    KeyRange r = from(std::move(v));
    r.low_exclusive_ = true;
    return r;
  }

  // [v, +∞)
  static KeyRange ge(Value v) {
    if (v.is_null()) {
      return empty();
    }
    return from(std::move(v));
  }

  // (-∞, v)：注意与 to(v) 的区别 —— 这里排除了 NULL
  static KeyRange lt(Value v) {
    if (v.is_null()) {
      return empty();
    }
    KeyRange r = non_null(v.type());
    r.high_ = std::move(v);
    r.high_inclusive_ = false;
    return r;
  }

  // (-∞, v]：同样排除 NULL
  static KeyRange le(Value v) {
    if (v.is_null()) {
      return empty();
    }
    KeyRange r = non_null(v.type());
    r.high_ = std::move(v);
    r.high_inclusive_ = true;
    return r;
  }

  // v = x（x 为 NULL 时等价于 IS NULL：null_only(type)）
  static KeyRange eq(Value v, DataType type = DataType::UNKNOWN_TYPE) {
    if (v.is_null()) {
      return null_only(type);
    }
    return point(v);
  }

  // 空集
  static KeyRange empty(DataType type = DataType::UNKNOWN_TYPE) {
    // 用 [NULL, NULL) 表示空集：low 排他、high 排他（不含），
    // 语义与类型都自洽（不依赖 Value(1)/Value(0) 这类魔法哨兵）。
    KeyRange r(type);
    r.low_ = Value();
    r.high_ = Value();
    r.low_exclusive_ = true;
    r.high_inclusive_ = false;
    return r;
  }

  // 有界区间 [start, end)
  // NULL 是具体边界（不是"无界"）：range(1, NULL) = [1, NULL) —— 没有值比 NULL
  // 更小，所以它是空集；要表达"无上界"请用 from(v)（或 all()）。
  static KeyRange range(Value start, Value end) {
    KeyRange r;
    // NULL 也作为具体边界保留（NULL 排在本族最前）
    r.low_ = std::move(start);
    r.high_ = std::move(end);
    r.type_ = !r.low_->is_null() ? r.low_->type() : r.high_->type();
    r.normalize(); // range(NULL, x) == 无下界
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
  // 注意：NULL 现在是"具体边界"，而没有任何值比 NULL 更小，
  // 所以 to(NULL) 视为空集（"无上界"请用 all() 或 from(v)）。
  static KeyRange to(Value end) {
    if (end.is_null()) {
      return empty();
    }
    KeyRange r;
    r.high_ = std::move(end);
    r.type_ = r.high_->type();
    return r;
  }

  // 单点区间 {v} = [v, v]
  //
  // 统一用"闭区间"表示，不再对整数用 [v, v+1)：
  //   - 避免 v == 类型最大值时的 +1 溢出；
  //   - 所有类型只有一种单点表示，压平成物理区间时 end = key(v)+0x00；
  //   - 要对 NULL 取单点请用 null_only(type)（这里不知道列类型）。
  static KeyRange point(const Value &v) {
    if (v.is_null()) {
      return empty();
    }
    return closed(v, v);
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
      return std::nullopt; // 无下界（-∞）
    }
    if (low_->is_null()) {
      // NULL 作为下界：包含它 = 从族最小值（NULL 的 key）开始；
      // 排除它 = 用"后继 key"表示（与 high 侧/其它边界的表示保持一致，
      // 这样 is_adjacent()/unite() 才能判定 (-∞,NULL] 与 (NULL,+∞) 相邻；
      // 等价的 [tag][0x01] 形式见 KeyCodecs::first_value_key）。
      Key k = KeyCodecs::null_key_for_type(type_);
      return low_exclusive_ ? KeyCodecs::inclusive_upper_bound(k) : k;
    }
    Key k = low_->to_key();
    if (k.empty()) {
      return std::nullopt;
    }
    if (low_exclusive_) {
      k = KeyCodecs::inclusive_upper_bound(k);
    }
    return k;
  }

  std::optional<Key> high_key() const {
    if (!high_) {
      return std::nullopt; // 无上界（+∞）
    }
    if (high_->is_null()) {
      Key k = KeyCodecs::null_key_for_type(type_);
      return high_inclusive_ ? KeyCodecs::inclusive_upper_bound(k) : k;
    }
    Key k = high_->to_key();
    if (k.empty()) {
      return std::nullopt;
    }
    if (high_inclusive_) {
      k = KeyCodecs::inclusive_upper_bound(k);
    }
    return k;
  }

  bool low_exclusive() const { return low_exclusive_; }
  bool high_inclusive() const { return high_inclusive_; }

  // ---- NULL 语义（供优化器按谓词选择/查询）----

  // 该区间是否会扫到 NULL
  bool includes_null() const { return null_scope() != NullScope::EXCLUDE; }

  // INCLUDE：含 NULL（默认，例如全表扫描 / 下界无界）
  // EXCLUDE：只有非 NULL 值（下界从第一个非 NULL 值开始，或下界是具体值）
  // ONLY   ：只有 NULL（单点）
  NullScope null_scope() const {
    if (is_empty()) {
      return NullScope::EXCLUDE;
    }
    const bool low_ok = !low_ || (low_->is_null() && !low_exclusive_);
    const bool high_ok = !high_ || !high_->is_null() || high_inclusive_;
    if (!(low_ok && high_ok)) {
      return NullScope::EXCLUDE;
    }
    // 上界就是 NULL（含）→ 只有 NULL
    if (high_ && high_->is_null() && high_inclusive_) {
      return NullScope::ONLY;
    }
    return NullScope::INCLUDE;
  }

  // ============================================================
  // 状态查询
  // ============================================================

  bool is_empty() const {
    // 上界是 NULL 且不含：没有值比 NULL 更小 → 空集
    // （否则 complement(null_only) 会多出一个"扫不到任何行"的退化区间）
    if (high_ && high_->is_null() && !high_inclusive_) {
      return true;
    }
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
    // (-∞, NULL]：本族里只有 NULL 的 key 落在其中
    if (!low_ && high_ && high_->is_null() && high_inclusive_) {
      return true;
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
    if (!is_point()) {
      return Value(); // null value
    }
    if (low_) {
      return low_exclusive_ ? *high_ : *low_;
    }
    return *high_; // (-∞, NULL] 的单点值就是 NULL
  }

  // ============================================================
  // 查询操作（全部基于编码后 key 的字节序）
  // ============================================================

  bool contains(const Value &v) const {
    if (is_empty()) {
      return false;
    }
    if (v.is_null()) {
      // NULL 是否落在区间内：看两侧边界对"族最小值"的态度
      const bool low_ok = !low_ || (low_->is_null() && !low_exclusive_);
      const bool high_ok = !high_ || !high_->is_null() || high_inclusive_;
      return low_ok && high_ok;
    }
    const Key k = v.to_key();
    if (k.empty()) {
      return false; // NULL / UNKNOWN 没有 key
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

    result.normalize();
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

    result.normalize();
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
      left.normalize();
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
      right.normalize();
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
      left.normalize();
      if (!left.is_empty()) {
        result.push_back(left);
      }
    }

    // 右补集：x > high 的部分
    if (high_) {
      KeyRange right(type_);
      right.low_ = high_;
      right.low_exclusive_ = high_inclusive_;
      right.normalize();
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
    if (null_scope() == NullScope::ONLY) {
      return "{NULL}";
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

  // 规范化：下界是"含 NULL"时，等价于"无下界"（都从族最小值开始），
  // 统一成 nullopt 形式，避免同一个区间出现两种结构表示
  // （例如 unite(null_only, non_null) 应该与 all() 结构一致）。
  // 注意：NULL **排他**的下界（non_null）不能规范化，它表达"跳过 NULL"。
  void normalize() {
    if (low_ && low_->is_null() && !low_exclusive_) {
      low_ = std::nullopt;
    }
  }

  // 类型合并：优先保留已知类型
  static DataType merge_type(DataType a, DataType b) {
    if (a != DataType::UNKNOWN_TYPE) {
      return a;
    }
    return b;
  }

  // 把 (Value, 开闭, NULL) 语义压平成物理半开区间 [start, end)
  StrKeyRange to_str_key_range_impl(DataType type) const {
    // 空集：[x, x)
    if (is_empty()) {
      const Key k = Value::min_key_for_type(type);
      return StrKeyRange{k, k};
    }

    // 下界：无下界(-∞) = 族最小值（NULL 的 key，含 NULL）；
    //       有界 = 具体 key，开区间则取"后继 key"
    Key start;
    if (!low_) {
      start = Value::min_key_for_type(type);
    } else {
      Key k = low_->is_null() ? Value::min_key_for_type(type) : low_->to_key();
      if (k.empty()) {
        k = Value::min_key_for_type(type);
      }
      start = low_exclusive_ ? KeyCodecs::inclusive_upper_bound(k) : k;
    }

    // 上界：无上界(+∞) = 族上界（本身已是排他）；
    //       有界 = 具体 key，闭区间则取"后继 key"
    Key end;
    if (!high_) {
      end = Value::upper_key_for_type(type);
    } else {
      Key k =
          high_->is_null() ? Value::min_key_for_type(type) : high_->to_key();
      if (k.empty()) {
        k = Value::upper_key_for_type(type);
      }
      end = high_inclusive_ ? KeyCodecs::inclusive_upper_bound(k) : k;
    }

    return StrKeyRange{start, end};
  }

  DataType type_ = DataType::UNKNOWN_TYPE;
  std::optional<Value> low_;    // nullopt = -∞
  std::optional<Value> high_;   // nullopt = +∞
  bool low_exclusive_ = false;  // 下界不含 low_
  bool high_inclusive_ = false; // 上界包含 high_
};

} // namespace sql
