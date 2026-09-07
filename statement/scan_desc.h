// scan_descriptor.h
#pragma once

#include "key_range.h"
#include "key_set.h"
#include <vector>
#include <string>

namespace query
{

  // ============================================================
  // 扫描描述：完整描述一次索引扫描
  // ============================================================
  struct ScanDescriptor
  {
    query::KeySet point_set;                // 点查询集合 (IN 列表)
    std::vector<query::KeyRange> range_set; // 范围集合
    query::KeySet excluded;                 // 需要排除的点 (NOT IN, !=)

    static ScanDescriptor empty()
    {
      return ScanDescriptor{};
    }
    static ScanDescriptor all()
    {

      ScanDescriptor desc;
      desc.range_set.push_back(KeyRange::all());
      return desc;
    }

    // 是否为空（无数据可扫描）
    bool is_empty() const
    {
      return point_set.empty() && range_set.empty();
    }

    // 是否为全表扫描
    bool is_all_scan() const
    {
      return range_set.size() == 1 &&
             range_set[0].is_all() &&
             point_set.empty() &&
             excluded.empty();
    }

    // 是否有点查询
    bool has_points() const { return !point_set.empty(); }

    // 是否有范围查询
    bool has_ranges() const { return !range_set.empty(); }

    // 是否有排除集
    bool has_excluded() const { return !excluded.empty(); }

    // 合并重叠范围
    void merge_ranges()
    {
      if (range_set.size() <= 1)
        return;

      // 排序：按 start 排序，无 start 排前面；start 相同按 end 排序，无 end 排后面
      std::sort(range_set.begin(), range_set.end(),
                [](const KeyRange &a, const KeyRange &b)
                {
                  // 无 start（-∞）排在前面
                  if (!a.has_start() && !b.has_start())
                  {
                    // 都无 start，按 end 排序（无 end 排后面）
                    if (a.has_end() && b.has_end())
                      return a.end() < b.end();
                    if (a.has_end())
                      return false;
                    if (b.has_end())
                      return true;
                    return false;
                  }
                  if (!a.has_start())
                    return true; // a.start < b.start
                  if (!b.has_start())
                    return false; // b.start < a.start, so !(a<b)

                  // start 不同
                  if (a.start() != b.start())
                  {
                    return a.start() < b.start();
                  }

                  // start 相同，按 end 排序（无 end 排后面）
                  if (a.has_end() && b.has_end())
                    return a.end() < b.end();
                  if (a.has_end())
                    return false;
                  if (b.has_end())
                    return true;
                  return false;
                });

      // 合并重叠范围
      std::vector<KeyRange> merged;
      for (const auto &range : range_set)
      {
        if (merged.empty())
        {
          merged.push_back(range);
          continue;
        }
        auto &last = merged.back();
        auto united = last.unite(range);
        if (united.is_nonempty())
        {
          last = united;
        }
        else
        {
          merged.push_back(range);
        }
      }
      range_set = std::move(merged);
    }

    // 规范化：将连续点集转为范围
    void normalize_points()
    {
      if (point_set.size() < 3)
        return;
      auto range = point_set.to_range();
      if (range && range->is_nonempty())
      {
        range_set.push_back(*range);
        point_set = KeySet();
      }
    }

    // 优化描述
    void optimize()
    {
      normalize_points();
      merge_ranges();
      // 如果排除集包含在点集或范围中，可以进一步优化
      // 这里简化处理
    }

    std::string to_string() const
    {
      std::string s = "ScanDescriptor{";
      bool first = true;
      if (has_points())
      {
        s += "points: " + point_set.to_string();
        first = false;
      }
      if (has_ranges())
      {
        if (!first)
          s += ", ";
        s += "ranges: [";
        for (size_t i = 0; i < range_set.size(); ++i)
        {
          if (i > 0)
            s += ", ";
          s += range_set[i].to_string();
        }
        s += "]";
        first = false;
      }
      if (has_excluded())
      {
        if (!first)
          s += ", ";
        s += "excluded: " + excluded.to_string();
      }
      if (is_empty())
      {
        s += "empty";
      }
      if (is_all_scan())
      {
        s += "all_scan";
      }
      s += "}";
      return s;
    }

    // ============================================================
    // AND 操作：交集
    // ============================================================
    static inline std::optional<ScanDescriptor> intersect(const ScanDescriptor &a, const ScanDescriptor &b)
    {
      ScanDescriptor result;

      // 1. 点集 ∩ 点集 = 点集
      result.point_set = a.point_set.intersect(b.point_set);

      // 2. 点集 ∩ 范围
      // 点集中落在范围内的保留
      for (const auto &p : a.point_set)
      {
        bool covered = false;
        for (const auto &r : b.range_set)
        {
          if (r.contains(p))
          {
            covered = true;
            break;
          }
        }
        if (covered)
          result.point_set.add(p);
      }
      for (const auto &p : b.point_set)
      {
        bool covered = false;
        for (const auto &r : a.range_set)
        {
          if (r.contains(p))
          {
            covered = true;
            break;
          }
        }
        if (covered)
          result.point_set.add(p);
      }

      // 3. 范围 ∩ 范围 = 范围（取交集）
      for (const auto &ra : a.range_set)
      {
        for (const auto &rb : b.range_set)
        {
          auto inter = ra.intersect(rb);
          if (inter.is_nonempty())
          {
            result.range_set.push_back(inter);
          }
        }
      }

      // 4. 排除集合并
      result.excluded = a.excluded.unite(b.excluded);

      // 5. 优化
      result.optimize();

      return result;
    }

    // ============================================================
    // OR 操作：并集
    // ============================================================
    static inline std::optional<ScanDescriptor> unite(const ScanDescriptor &a, const ScanDescriptor &b)
    {
      ScanDescriptor result;

      // 1. 点集合并
      result.point_set = a.point_set.unite(b.point_set);

      // 2. 范围合并
      result.range_set = a.range_set;
      result.range_set.insert(result.range_set.end(),
                              b.range_set.begin(), b.range_set.end());

      // 3. 排除集合并
      result.excluded = a.excluded.unite(b.excluded);

      // 4. 优化
      result.optimize();

      return result;
    }

    // ============================================================
    // NOT 操作：补集
    // ============================================================
    static inline std::optional<ScanDescriptor> complement(const ScanDescriptor &desc)
    {
      // 如果描述为空，返回全范围
      if (desc.is_empty())
      {
        return ScanDescriptor::all();
      }

      // 如果描述已经是全范围，返回空
      if (desc.is_all_scan())
      {
        return ScanDescriptor::empty();
      }

      // 多个点
      if (desc.has_points() && !desc.has_ranges())
      {
        // 多个点的补集 = 全范围排除这些点
        // 可以用 KeySet 表示，但需要保证排除集能精确表示
        auto result = ScanDescriptor::all();
        result.excluded = desc.point_set;
        result.point_set = desc.excluded;
        return result;
      }

      // 如果描述是单个范围，返回补集（可能两个范围）
      if (desc.range_set.size() == 1 && desc.point_set.empty())
      {
        const auto &range = desc.range_set[0];
        if (range.is_all())
        {
          // 全范围取补 = 空
          return ScanDescriptor::empty();
        }
        ScanDescriptor result;
        // 补集可能产生两个范围：[−∞, start) 和 [end, +∞)
        auto complements = range.complement();
        // ✅ 将补集的所有范围都加入 range_set
        for (const auto &comp : complements)
        {
          if (comp.is_nonempty())
          {
            result.range_set.push_back(comp);
          }
        }
        return result;
      }

      // 多个range
      if (desc.range_set.size() > 1)
      {
        auto cloned = desc;
        cloned.optimize();
        auto &sorted = cloned.range_set;
        ScanDescriptor result;

        // 第一段之前
        if (sorted[0].has_start())
        {
          result.range_set.push_back(KeyRange::to(sorted[0].start()));
        }

        // 中间段
        for (size_t i = 0; i + 1 < sorted.size(); ++i)
        {
          const auto &left = sorted[i];
          const auto &right = sorted[i + 1];

          if (left.has_end() && right.has_start())
          {
            // 检查是否有空隙
            if (left.end() < right.start())
            {
              result.range_set.push_back(
                  KeyRange::range(left.end(), right.start()));
            }
            // 如果 left.end >= right.start，说明范围重叠或相邻
            // 这种情况下补集为空，不添加
          }
        }

        // 最后一段之后
        const auto &last = sorted.back();
        if (last.has_end())
        {
          result.range_set.push_back(KeyRange::from(last.end()));
        }

        result.point_set = desc.excluded;
        result.excluded = desc.point_set;

        return result;
      }

      // 复杂情况：返回空（无法精确表示补集）
      return std::nullopt;
    }

  }; // struct ScanDescriptor
} // namespace query