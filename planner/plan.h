// plan.h
//
// Plan 树：OptimizedQuery -> 交给执行器（Volcano 模型）的计划节点。
//
// ============================================================
// 算子词汇表
//
//   数据访问：FullScan / IndexScanPlan（单个区间）/
//   RangeUnionPlan（多区间拼接） 行处理：  Filter / Sort(TopN) / Limit /
//   Project 写与元数据：Insert / Update / Delete / DDL / USE
//
// 每类语句的链（公共的"扫描链"只搭一次，见 planner.cpp 的 build_scan_chain）：
//
//   SELECT:  Project? -> Limit? -> Sort/TopN? -> Filter? -> RangeUnion? ->
//   FullScan/IndexScan UPDATE:  Update   -> Limit? -> Sort?     -> Filter? ->
//   RangeUnion? -> FullScan/IndexScan DELETE:  Delete   -> Limit? -> Sort? ->
//   Filter? -> RangeUnion? -> FullScan/IndexScan INSERT:  Insert（child 为空 =
//   行源就是语句里的 VALUES；将来
//                    INSERT ... SELECT 时 child 挂一条 SELECT 链）
//   DDL/USE: 叶子（没有行流）
//
// ============================================================
// 两条约定
//
// 1) **建树时"搬家"**：OptimizedQuery 的扫描空间（ranges / exclude_keys）与
//    过滤条件（remaining_filter）在 Planner 里被**移进**对应算子
//    （RangeUnionPlan / FilterPlan），避免同一份数据在树里有第二个真相；
//    节点上的字段才是执行器该读的。
//
// 2) **谁持有语句**：sql::Query 是 move-only，整棵树里最多一个节点持有它 ——
//    写语句与 DDL 节点持有（它们需要 assignments / 库表名），
//    扫描节点不持有，改用轻量的 TableRef（见下）。
//
// 3) PlanNode 只描述"怎么做"，**不保存运行时状态**；游标状态归执行器。
//    to_string() 只用于调试/日志/EXPLAIN，不参与任何语义判断。
#ifndef QUERY_PLAN_H
#define QUERY_PLAN_H

#include <cstddef>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "planner/optimizer.h"    // OptimizedQuery
#include "planner/planner_defs.h" // PlanError / PlanErrorCode / Ordering
#include "sql_types/query.h"

namespace plan {

// ============================================================
// 计划节点类型
// ============================================================
enum class PlanType {
  FULL_SCAN,   // 全表扫描
  INDEX_SCAN,  // 索引扫描：一个具体区间（点查询就是退化区间）
  RANGE_UNION, // 多个区间按序拼接（离散集合 -> 连续行流的状态机在这里）
  FILTER,      // 条件过滤
  SORT,        // 排序（可能是 TopN）
  LIMIT,       // LIMIT / OFFSET
  PROJECT,     // 投影
  INSERT,      // 插入
  UPDATE,      // 更新
  DELETE,      // 删除
  CREATE_DATABASE,
  DROP_DATABASE,
  CREATE_TABLE,
  DROP_TABLE,
  USE_DATABASE
};

inline const char *plan_type_to_string(PlanType type) {
  switch (type) {
  case PlanType::FULL_SCAN:
    return "FULL_SCAN";
  case PlanType::INDEX_SCAN:
    return "INDEX_SCAN";
  case PlanType::RANGE_UNION:
    return "RANGE_UNION";
  case PlanType::FILTER:
    return "FILTER";
  case PlanType::SORT:
    return "SORT";
  case PlanType::LIMIT:
    return "LIMIT";
  case PlanType::PROJECT:
    return "PROJECT";
  case PlanType::INSERT:
    return "INSERT";
  case PlanType::UPDATE:
    return "UPDATE";
  case PlanType::DELETE:
    return "DELETE";
  case PlanType::CREATE_DATABASE:
    return "CREATE_DATABASE";
  case PlanType::DROP_DATABASE:
    return "DROP_DATABASE";
  case PlanType::CREATE_TABLE:
    return "CREATE_TABLE";
  case PlanType::DROP_TABLE:
    return "DROP_TABLE";
  case PlanType::USE_DATABASE:
    return "USE_DATABASE";
  }
  return "UNKNOWN";
}

// ============================================================
// 扫描算子的"表定位"信息
//
// 只要这些就能打开一张表（执行器拿它去 catalog.open_table）：
// 主键类型用于 key 编解码自检与 EXPLAIN。Identifier 很小，
// 每个扫描节点各带一份，换来算子自包含、可单独构造与测试。
// ============================================================
struct TableRef {
  sql::Identifier db;
  sql::Identifier table;
  sql::Identifier primary_key; // 空 = 表无主键（只能全表扫描）
  sql::DataType primary_key_type = sql::DataType::UNKNOWN_TYPE;

  bool has_primary_key() const { return !primary_key.empty(); }

  std::string to_string() const {
    std::string s = table.empty() ? "?" : table.str();
    if (has_primary_key()) {
      s += " pk=" + primary_key.str();
      s += " " + std::string(sql::data_type_name(primary_key_type));
    }
    return s;
  }
};

// ============================================================
// 执行计划基类
// ============================================================
class PlanNode {
public:
  virtual ~PlanNode() = default;

  virtual PlanType type() const = 0;
  virtual std::string to_string() const = 0;

  // 输出顺序：Sort 消除的判据，也是游标正/反向迭代的依据
  virtual Ordering output_order() const { return Ordering::NONE; }

  // 子节点；叶子返回 nullptr
  virtual const PlanNode *child() const { return nullptr; }
};

// ============================================================
// 扫描算子：候选区间 + 扫描方向
//
// 区间恒按主键升序（见 OptimizedQuery）；降序不翻转区间数组，
// 而是把 ascending 置 false，由游标反向迭代（SeekForPrev + Prev）。
// ============================================================
class ScanPlan : public PlanNode {
public:
  ScanPlan(TableRef target, bool ascending)
      : target_(std::move(target)), ascending_(ascending) {}

  const TableRef &target() const { return target_; }
  bool ascending() const { return ascending_; }

  Ordering output_order() const override {
    return ascending_ ? Ordering::PK_ASC : Ordering::PK_DESC;
  }

protected:
  TableRef target_;
  bool ascending_ = true;
};

// 全表扫描（候选空间就是整张表，没有要跳过的点）
class FullScan : public ScanPlan {
public:
  FullScan(TableRef target, bool ascending = true)
      : ScanPlan(std::move(target), ascending) {}
  PlanType type() const override { return PlanType::FULL_SCAN; }
  std::string to_string() const override;
};

// 单区间扫描：点查询就是退化区间 [v, v]
class IndexScanPlan : public ScanPlan {
public:
  IndexScanPlan(TableRef target, sql::KeyRange range, bool ascending = true)
      : ScanPlan(std::move(target), ascending), range_(std::move(range)) {}

  PlanType type() const override { return PlanType::INDEX_SCAN; }
  std::string to_string() const override;

  const sql::KeyRange &range() const { return range_; }
  // 点查询（执行器可以直接退化成一次 Get）
  bool is_point() const { return range_.is_point(); }

private:
  sql::KeyRange range_;
};

// ============================================================
// RangeUnionPlan：多区间拼接
//
// "优化器给出的是离散区间集合，但要对外提供连续的行流"——那个状态机
// 住在这里（而不是焊在扫描节点内部），所以它可以被单独测试：
// 正/反向、空区间、相邻区间、LIMIT 早停。
//
//   - ranges_ 恒有序（主键升序）、互不相交（optimizer 保证）；
//   - ascending_ == false 时按 ranges_ 逆序、每个区间反向扫；
//   - exclude_keys_ 是扫描提示（<> / NOT IN 的主键点），归并式跳过；
//     对应的谓词仍在 Filter 里做兜底。
// ============================================================
class RangeUnionPlan : public ScanPlan {
public:
  RangeUnionPlan(TableRef target, std::vector<sql::KeyRange> ranges,
                 bool ascending, sql::KeySet exclude_keys)
      : ScanPlan(std::move(target), ascending), ranges_(std::move(ranges)),
        exclude_keys_(std::move(exclude_keys)) {}

  PlanType type() const override { return PlanType::RANGE_UNION; }
  std::string to_string() const override;

  const std::vector<sql::KeyRange> &ranges() const { return ranges_; }
  const sql::KeySet &exclude_keys() const { return exclude_keys_; }

private:
  std::vector<sql::KeyRange> ranges_;
  sql::KeySet exclude_keys_;
};

// ============================================================
// Filter：对每一行按 SQL 三值逻辑求值，只放行 TRUE
// ============================================================
class FilterPlan : public PlanNode {
public:
  FilterPlan(std::unique_ptr<PlanNode> child, sql::ConditionPtr condition)
      : child_(std::move(child)), condition_(std::move(condition)) {}

  PlanType type() const override { return PlanType::FILTER; }
  std::string to_string() const override;

  // 过滤不改变行的顺序
  Ordering output_order() const override {
    return child_ ? child_->output_order() : Ordering::NONE;
  }
  const PlanNode *child() const override { return child_.get(); }
  const sql::Condition *condition() const { return condition_.get(); }

private:
  std::unique_ptr<PlanNode> child_;
  sql::ConditionPtr condition_;
};

// ============================================================
// Sort / TopN
//
//   is_top_n == false : 全量排序（没有 LIMIT）
//   is_top_n == true  : 只需要前 top_n 行（= OFFSET + LIMIT），
//                       执行器用大小为 top_n 的堆，单遍扫描即可
//
// 比较规则（执行器实现）：按 order_by 逐列比较（NULL 最小），
// 末尾追加主键做 tiebreaker —— 否则并列时 LIMIT 取哪几行不确定。
// ============================================================
class SortPlan : public PlanNode {
public:
  SortPlan(std::unique_ptr<PlanNode> child,
           std::vector<sql::OrderByItem> order_by, bool is_top_n = false,
           size_t top_n = 0)
      : child_(std::move(child)), order_by_(std::move(order_by)),
        is_top_n_(is_top_n), top_n_(top_n) {}

  PlanType type() const override { return PlanType::SORT; }
  std::string to_string() const override;

  // 排序后的顺序不再等于主键顺序（否则 Planner 会消掉这个节点）
  Ordering output_order() const override { return Ordering::NONE; }
  const PlanNode *child() const override { return child_.get(); }

  const std::vector<sql::OrderByItem> &order_by() const { return order_by_; }
  bool is_top_n() const { return is_top_n_; }
  size_t top_n() const { return top_n_; }

private:
  std::unique_ptr<PlanNode> child_;
  std::vector<sql::OrderByItem> order_by_;
  bool is_top_n_ = false;
  size_t top_n_ = 0;
};

// ============================================================
// Limit：跳过 offset 行后返回 limit 行（提前停止）
// ============================================================
class LimitPlan : public PlanNode {
public:
  LimitPlan(std::unique_ptr<PlanNode> child, const sql::LimitClause &limit)
      : child_(std::move(child)), limit_(limit) {}

  PlanType type() const override { return PlanType::LIMIT; }
  std::string to_string() const override;

  Ordering output_order() const override {
    return child_ ? child_->output_order() : Ordering::NONE;
  }
  const PlanNode *child() const override { return child_.get(); }
  const sql::LimitClause &limit() const { return limit_; }

private:
  std::unique_ptr<PlanNode> child_;
  sql::LimitClause limit_;
};

// ============================================================
// Projection：只保留需要的列（SELECT * 时不建这个节点）
// ============================================================
class ProjectPlan : public PlanNode {
public:
  ProjectPlan(std::unique_ptr<PlanNode> child,
              std::vector<sql::ColumnRef> columns)
      : child_(std::move(child)), columns_(std::move(columns)) {}

  PlanType type() const override { return PlanType::PROJECT; }
  std::string to_string() const override;

  Ordering output_order() const override {
    return child_ ? child_->output_order() : Ordering::NONE;
  }
  const PlanNode *child() const override { return child_.get(); }
  const std::vector<sql::ColumnRef> &columns() const { return columns_; }

private:
  std::unique_ptr<PlanNode> child_;
  std::vector<sql::ColumnRef> columns_;
};

// ============================================================
// 持有 sql::Query 的节点（写语句 / DDL）
//
// Query 是 move-only：整棵树里只有这些节点持有它（assignments、VALUES、
// 库表名都在语句里），扫描链改用 TableRef。
// ============================================================
class QueryPlanNode : public PlanNode {
public:
  explicit QueryPlanNode(OptimizedQuery query) : query_(std::move(query)) {}

  const OptimizedQuery &query() const { return query_; }

protected:
  OptimizedQuery query_;
};

// UPDATE：拉一行、改一行；数据从 child 来（扫描链与 SELECT 完全共用）
class UpdatePlan : public QueryPlanNode {
public:
  UpdatePlan(OptimizedQuery query, std::unique_ptr<PlanNode> child)
      : QueryPlanNode(std::move(query)), child_(std::move(child)) {}
  PlanType type() const override { return PlanType::UPDATE; }
  std::string to_string() const override;
  const PlanNode *child() const override { return child_.get(); }

private:
  std::unique_ptr<PlanNode> child_;
};

// DELETE：拉一行、删一行
class DeletePlan : public QueryPlanNode {
public:
  DeletePlan(OptimizedQuery query, std::unique_ptr<PlanNode> child)
      : QueryPlanNode(std::move(query)), child_(std::move(child)) {}
  PlanType type() const override { return PlanType::DELETE; }
  std::string to_string() const override;
  const PlanNode *child() const override { return child_.get(); }

private:
  std::unique_ptr<PlanNode> child_;
};

// INSERT：行源就是语句里的 VALUES，所以 child 为空；
// 将来支持 INSERT ... SELECT 时，child 挂那条 SELECT 链。
class InsertPlan : public QueryPlanNode {
public:
  explicit InsertPlan(OptimizedQuery query,
                      std::unique_ptr<PlanNode> child = nullptr)
      : QueryPlanNode(std::move(query)), child_(std::move(child)) {}
  PlanType type() const override { return PlanType::INSERT; }
  std::string to_string() const override;
  const PlanNode *child() const override { return child_.get(); }

private:
  std::unique_ptr<PlanNode> child_;
};

// ============================================================
// DDL / USE：没有行流，叶子
// ============================================================
class CreateDatabasePlan : public QueryPlanNode {
public:
  explicit CreateDatabasePlan(OptimizedQuery query)
      : QueryPlanNode(std::move(query)) {}
  PlanType type() const override { return PlanType::CREATE_DATABASE; }
  std::string to_string() const override;
};

class DropDatabasePlan : public QueryPlanNode {
public:
  explicit DropDatabasePlan(OptimizedQuery query)
      : QueryPlanNode(std::move(query)) {}
  PlanType type() const override { return PlanType::DROP_DATABASE; }
  std::string to_string() const override;
};

class CreateTablePlan : public QueryPlanNode {
public:
  explicit CreateTablePlan(OptimizedQuery query)
      : QueryPlanNode(std::move(query)) {}
  PlanType type() const override { return PlanType::CREATE_TABLE; }
  std::string to_string() const override;
};

class DropTablePlan : public QueryPlanNode {
public:
  explicit DropTablePlan(OptimizedQuery query)
      : QueryPlanNode(std::move(query)) {}
  PlanType type() const override { return PlanType::DROP_TABLE; }
  std::string to_string() const override;
};

class UseDatabasePlan : public QueryPlanNode {
public:
  explicit UseDatabasePlan(OptimizedQuery query)
      : QueryPlanNode(std::move(query)) {}
  PlanType type() const override { return PlanType::USE_DATABASE; }
  std::string to_string() const override;
};

// 缩进打印整棵树（调试 / EXPLAIN 用）：每个节点一行，按 child() 向下走
std::string plan_tree_to_string(const PlanNode &root);

} // namespace plan

#endif // QUERY_PLAN_H
