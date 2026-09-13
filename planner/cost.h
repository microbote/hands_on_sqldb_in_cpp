// cost.h
//
// 极简 cost model —— **占位性质**：目的是让"统计 -> 成本 -> 选计划"
// 这个环节真实存在于链路里，而不是追求准确。
//
//   - 只有一对数字 (startup, total)：LIMIT 只关心 startup（能不能早停），
//     其余关心 total（跑完要多少）；
//   - 目前唯一的决策点：**稀疏点集要不要下推**（见 optimizer.cpp）——
//     点查 k 个点 vs 全表扫 N 行，谁便宜选谁；
//   - 常数是相对量（都按"扫一行"为 1 个单位），故意取得简单好解释；
//   - 统计缺失（rows_known == false）时**不做决策**，退回原来的规则行为，
//     避免用垃圾数据改变计划。
//
// 将来有二级索引/join 时，这里会换成"每个算子给出一对数 + 候选枚举"，
// 现在的 CostModel 就是那个骨架。
#pragma once

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <string>

#include "sql_types/identifier.h"

namespace plan {

// ============================================================
// 成本：两个数字（相对量）
// ============================================================
struct Cost {
  double startup = 0.0; // 拿到第一行要多少
  double total = 0.0;   // 全部拿完要多少

  std::string to_string() const {
    char buffer[64];
    std::snprintf(buffer, sizeof(buffer), "%.1f..%.1f", startup, total);
    return buffer;
  }
};

// ============================================================
// 统计供给：planner 只认识这个函数，不依赖具体 Catalog 实现
//
// session 用 KVCatalog 的统计表实现它（那里有维护好的行数），
// 这样 sql_types 的 Catalog 接口不用改。
// ============================================================
struct RelationStats {
  int64_t rows = 0;
  bool rows_known = false; // false = 没有统计，成本模型不参与决策
};

using StatsProvider = std::function<RelationStats(
    const sql::Identifier &db, const sql::Identifier &table)>;

// ============================================================
// 成本常数（相对量：扫一行 == 1.0）
// ============================================================
struct CostConstants {
  // 一次 seek ≈ 扫 10 行（经典量级假设：随机读比顺序读贵一个数量级）。
  // 这个数决定"点集多大就不划算了"：约 k > N/5 时退化成全表扫。
  double seek = 10.0;
  double seq_row = 1.0;    // 顺序扫一行
  double point_row = 1.0;  // 点查命中一行
  double filter_row = 1.0; // 对一行求值一次谓词
  double sort_row = 1.0;   // 排序里一行的比较/搬运（log 因子单独算）
  double output_row = 0.5; // 往上层交一行
};

class CostModel {
public:
  CostModel() = default;
  explicit CostModel(CostConstants constants) : c_(constants) {}

  const CostConstants &constants() const { return c_; }

  // 全表扫：startup 0，total = 行数
  Cost full_scan(const RelationStats &stats) const {
    const double rows = static_cast<double>(stats.rows);
    return Cost{0.0, rows * c_.seq_row};
  }

  // k 个点查：k 次 seek + 最多 k 行
  Cost point_lookups(int64_t points) const {
    const double k = static_cast<double>(points);
    return Cost{k * c_.seek, k * (c_.seek + c_.point_row)};
  }

  // 区间扫描：一次 seek + 区间内的行
  Cost index_scan(double estimated_rows) const {
    return Cost{c_.seek, c_.seek + estimated_rows * c_.seq_row};
  }

  // ---- 上层算子：在子计划成本上叠加 ----
  Cost filter(const Cost &child, double rows_in,
              double selectivity = 1.0) const {
    return Cost{child.startup,
                child.total + rows_in * c_.filter_row * selectivity};
  }
  Cost sort(const Cost &child, double rows) const {
    const double comparisons = rows > 1 ? rows * std::log2(rows) : rows;
    const double sorted = child.total + comparisons * c_.sort_row;
    return Cost{sorted, sorted + rows * c_.output_row};
  }
  Cost top_n(const Cost &child, double rows, double keep) const {
    const double comparisons = keep > 0 ? rows * std::log2(keep + 1) : 0.0;
    return Cost{child.total + comparisons * c_.sort_row,
                child.total + comparisons * c_.sort_row + keep * c_.output_row};
  }
  Cost limit(const Cost &child, double rows_out) const {
    return Cost{child.startup, child.startup + rows_out * c_.output_row};
  }
  Cost project(const Cost &child, double rows) const {
    return Cost{child.startup, child.total + rows * c_.output_row};
  }

private:
  CostConstants c_;
};

} // namespace plan
