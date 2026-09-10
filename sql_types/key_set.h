// key_set.h
#pragma once

#include <algorithm>
#include <cstddef>
#include <initializer_list>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>
#include <iterator>

#include "key_range.h"
#include "value.h"

namespace sql {

// ============================================================
// KeySet：基于 std::vector 的点集，支持集合运算
// 特性：
//   - 惰性排序（const 方法中也可以排序）
//   - 自动去重
//   - 集合运算（并集、交集、差集、对称差集）
//   - 转换为 KeyRange（连续点集）
//   - 拆分为多个 KeyRange
// ============================================================
class KeySet {
 public:
  // ============================================================
  // 构造
  // ============================================================
  KeySet() = default;

  KeySet(std::initializer_list<sql::Value> init) : points_(init) {}

  explicit KeySet(const std::vector<sql::Value>& points) : points_(points) {}

  explicit KeySet(std::vector<sql::Value>&& points)
      : points_(std::move(points)) {}

  // ============================================================
  // 访问器
  // ============================================================
  const std::vector<sql::Value>& points() const {
    ensure_sorted();
    return points_;
  }

  // 非 const 版本会暴露可变引用：无法知道调用方是否修改内容，
  // 因此必须让惰性排序标志失效。否则 contains()/equals() 里的
  // binary_search 会在未排序的数据上给出假阴性。
  std::vector<sql::Value>& points() {
    sorted_ = false;
    return points_;
  }
  bool empty() const { return points_.empty(); }
  bool is_empty() const { return points_.empty(); }
  size_t size() const { return points_.size(); }

  // ============================================================
  // 插入（自动去重）
  // ============================================================
  KeySet& add(const sql::Value& val) {
    if (std::find(points_.begin(), points_.end(), val) == points_.end()) {
      points_.push_back(val);
      sorted_ = false;
    }
    return *this;
  }

  KeySet& add_all(const std::vector<sql::Value>& vals) {
    for (const auto& v : vals) {
      add(v);
    }
    return *this;
  }

  KeySet& add_all(std::initializer_list<sql::Value> vals) {
    for (const auto& v : vals) {
      add(v);
    }
    return *this;
  }

  // 从另一个 KeySet 添加
  KeySet& add_all(const KeySet& other) {
    for (const auto& v : other.points_) {
      add(v);
    }
    return *this;
  }
// 删除单值
   bool remove(const sql::Value& val) {
     auto it = std::find(points_.begin(), points_.end(), val);
     if (it != points_.end()) {
       points_.erase(it);
       return true;
     }
     return false;
   }
   
   // 清空
   void clear() {
     points_.clear();
     sorted_ = true;  // 空集是已排序的
   }
  // ============================================================
  // 排序和去重（原地操作）
  // ============================================================
  KeySet& sort() {
    std::sort(points_.begin(), points_.end());
    sorted_ = true;
    return *this;
  }

  KeySet& unique() {
    sort();
    points_.erase(std::unique(points_.begin(), points_.end()), points_.end());
    sorted_ = true;
    return *this;
  }

  KeySet& normalize() {
    sort();
    points_.erase(std::unique(points_.begin(), points_.end()), points_.end());
    sorted_ = true;
    return *this;
  }

  // ============================================================
  // 惰性排序（const 方法中调用）
  // ============================================================
  void ensure_sorted() const {
    if (!sorted_ && points_.size() > 1) {
      std::sort(points_.begin(), points_.end());
      sorted_ = true;
    }
  }

  bool is_sorted() const { return sorted_; }

  // 最小值（请在 points_ 非空时调用）
  sql::Value min() const {
     if (points_.empty()) {
       throw std::runtime_error("KeySet::min() on empty set");
     }
     ensure_sorted();
     return points_.front();
   }
   
   // 最大值
   sql::Value max() const {
     if (points_.empty()) {
       throw std::runtime_error("KeySet::max() on empty set");
     }
     ensure_sorted();
     return points_.back();
   }
  // ============================================================
  // 集合运算（返回新的 KeySet）
  // ============================================================

  // 并集
  KeySet unite(const KeySet& other) const {
    ensure_sorted();
    other.ensure_sorted();
    std::vector<sql::Value> result;
    std::set_union(points_.begin(), points_.end(), other.points_.begin(),
                   other.points_.end(), std::back_inserter(result));
    return KeySet(std::move(result), true);
  }

  // 交集
  KeySet intersect(const KeySet& other) const {
    ensure_sorted();
    other.ensure_sorted();
    std::vector<sql::Value> result;
    std::set_intersection(points_.begin(), points_.end(), other.points_.begin(),
                          other.points_.end(), std::back_inserter(result));
    return KeySet(std::move(result), true);
  }

  // 差集: *this - other
  KeySet subtract(const KeySet& other) const {
    ensure_sorted();
    other.ensure_sorted();
    std::vector<sql::Value> result;
    std::set_difference(points_.begin(), points_.end(), other.points_.begin(),
                        other.points_.end(), std::back_inserter(result));
    return KeySet(std::move(result), true);
  }

  // 对称差集: (A - B) ∪ (B - A)
  KeySet symmetric_difference(const KeySet& other) const {
    ensure_sorted();
    other.ensure_sorted();
    std::vector<sql::Value> result;
    std::set_symmetric_difference(points_.begin(), points_.end(),
                                  other.points_.begin(), other.points_.end(),
                                  std::back_inserter(result));
    return KeySet(std::move(result), true);
  }

  // ============================================================
  // 原地集合操作
  // ============================================================

  KeySet& unite_with(const KeySet& other) {
    *this = unite(other);
    return *this;
  }

  KeySet& intersect_with(const KeySet& other) {
    *this = intersect(other);
    return *this;
  }

  KeySet& subtract_with(const KeySet& other) {
    *this = subtract(other);
    return *this;
  }

  // ============================================================
  // 查询
  // ============================================================

  bool contains(const sql::Value& val) const {
    if (points_.empty()) { return false;
}
    ensure_sorted();
    return std::binary_search(points_.begin(), points_.end(), val);
  }

  bool contains_all(const KeySet& other) const {
    if (other.empty()) { return true;
}
    ensure_sorted();
    other.ensure_sorted();
    return std::includes(points_.begin(), points_.end(), other.points_.begin(),
                         other.points_.end());
  }

  bool contains_any(const KeySet& other) const {
    if (empty() || other.empty()) { return false;
}
    ensure_sorted();
    other.ensure_sorted();
    std::vector<sql::Value> result;
    std::set_intersection(points_.begin(), points_.end(), other.points_.begin(),
                          other.points_.end(), std::back_inserter(result));
    return !result.empty();
  }

  bool equals(const KeySet& other) const {
    if (size() != other.size()) { return false;
}
    ensure_sorted();
    other.ensure_sorted();
    return points_ == other.points_;
  }

  // ============================================================
  // 类型检查
  // ============================================================

  bool all_int() const {
    ensure_sorted();
    return std::all_of(points_.begin(), points_.end(),
                       [](const sql::Value& v) { return v.is_int(); });
  }

  bool all_string() const {
    ensure_sorted();
    return std::all_of(points_.begin(), points_.end(),
                       [](const sql::Value& v) { return v.is_string(); });
  }

  // ============================================================
  // 转换为 KeyRange（连续点集）
  // ============================================================
  std::optional<KeyRange> to_range() const {
    if (points_.empty()) { return KeyRange::empty();
}
    if (!all_int()) { return std::nullopt;
}

    ensure_sorted();

    // 检查是否连续
    for (size_t i = 1; i < points_.size(); ++i) {
      if (points_[i].as_int() - points_[i - 1].as_int() != 1) {
        return std::nullopt;
      }
    }

    return KeyRange::range(points_.front(),
                           sql::Value(points_.back().as_int() + 1));
  }

  // 将点集拆分为多个连续范围
  std::vector<KeyRange> to_ranges() const {
    std::vector<KeyRange> result;
    if (points_.empty()) { return result;
}
    if (!all_int()) { return result;
}

    ensure_sorted();

    sql::Value start = points_[0];
    sql::Value current = start;

    for (size_t i = 1; i < points_.size(); ++i) {
      if (points_[i].as_int() - current.as_int() != 1) {
        result.push_back(
            KeyRange::range(start, sql::Value(current.as_int() + 1)));
        start = points_[i];
      }
      current = points_[i];
    }
    result.push_back(KeyRange::range(start, sql::Value(current.as_int() + 1)));

    return result;
  }

  // 是否有与 KeyRange 重叠
   bool is_intersect(const KeyRange& range) const {
     if (points_.empty() || range.is_empty()) return false;
     ensure_sorted();
     
     // 直接用 range 内部逻辑，让 range 处理无穷
     return range.intersects_set(*this);
   }

   // 过滤出范围内的点
   KeySet filter(const KeyRange& range) const {
     if (points_.empty() || range.is_empty()) {
       return {};
     }
     
     ensure_sorted();
     return range.filter_set(*this);
   }
  // ============================================================
  // 迭代器支持
  // ============================================================
  // 只提供只读迭代（按序）。需要修改内容请用 add/remove/points()，
  // 它们会正确维护排序标志。
  auto begin() const {
    ensure_sorted();
    return points_.cbegin();
  }
  auto end() const { return points_.cend(); }

   // 反转（用于降序遍历）
   auto rbegin() const {
     ensure_sorted();
     return points_.crbegin();
   }
   auto rend() const { return points_.crend(); }
   
   // 迭代器访问（直接通过容器）
   const sql::Value& at(size_t i) const {
     ensure_sorted();
     return points_.at(i);
   }
   
   // 随机访问操作符
   const sql::Value& operator[](size_t i) const {
     ensure_sorted();
     return points_[i];
   }
  // ============================================================
  // 序列化
  // ============================================================
  std::string to_string() const {
    if (points_.empty()) { return "{}";
}
    ensure_sorted();
    std::string s = "{";
    for (size_t i = 0; i < points_.size(); ++i) {
      if (i > 0) s += ", ";
      s += points_[i].to_string();
    }
    s += "}";
    return s;
  }

  // 完整字符串（不排序）
   std::string to_string_raw() const {
     std::string s = "[";
     for (size_t i = 0; i < points_.size(); ++i) {
       if (i > 0) s += ", ";
       s += points_[i].to_string();
     }
     s += "]";
     return s;
   }
   
   // 类型检查辅助（用于测试）
   DataType data_type() const {
     if (points_.empty()) return DataType::UNKNOWN_TYPE;
     return points_.front().type();
   }

 private:
  // 私有构造函数（用于已知已排序的情况）
  KeySet(std::vector<sql::Value>&& points, bool sorted)
      : points_(std::move(points)), sorted_(sorted) {}

  mutable std::vector<sql::Value> points_;
  mutable bool sorted_ = false;
};

// ============================================================
// 比较操作符
// ============================================================
inline bool operator==(const KeySet& a, const KeySet& b) { return a.equals(b); }

inline bool operator!=(const KeySet& a, const KeySet& b) {
  return !a.equals(b);
}

}  // namespace query
