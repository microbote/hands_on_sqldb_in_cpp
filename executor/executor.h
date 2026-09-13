// executor.h
//
// 执行器（Volcano 模型）：
//
//   计划树（plan.h） --ExecutorFactory--> 算子树 --ResultCursor--> 客户端
//
// 分工（见与 planner 的约定）：
//   - Executor 是**框架内部**的算子接口：open / next / close / plan，
//     客户端看不到它，也不应该看到 plan 节点；
//   - ResultCursor 是**客户端门面**：实现 sql::Cursor（next / close），
//     内部驱动根算子 —— "Cursor::next() 驱动执行器干活"；
//   - 行流的结束用 sql::CursorErrorCode::END 表示（见 sql_types/cursor.h），
//     所以 next() 只有一个返回通道，不需要 optional + error 两套。
//
// 与计划节点的对应关系（每个算子只做一件事）：
//
//   FullScan / IndexScan / RangeUnion -> ScanExecutor（多区间 + 方向 + 跳点）
//   Filter                            -> FilterExecutor
//   Sort(TopN)                        -> SortExecutor（有 LIMIT 时只留 n 行堆）
//   Limit                             -> LimitExecutor
//   Project                           -> ProjectExecutor
//   Update / Delete / Insert          -> 写算子（拉一行改/删一行；INSERT
//   无子节点）
//
// DDL / USE 不在这里执行：它们没有行流，由上层用 sql::Catalog 完成
// （USE 还需要会话状态，不在 Catalog 接口里）。
//
// **统计（EXPLAIN ANALYZE）**：`open/next/close` 是非虚的包装，
// 里面调算子的 `open_impl/next_impl/close_impl` —— 这样"数行数/记时间"
// 只写一处，算子实现不用操心。只有传了 ExecReport 才统计，平时零开销。
//
// **生命周期**：算子在构造时就把运行期要用的语义载荷（谓词、order_by、
// LIMIT、投影列、SET 列表、VALUES）**拷成自己的成员**，运行期不再解引用
// 计划节点 —— 计划树可以比游标先释放（planner 的产物是"一次性"的）。
// `plan()` 只在计划树仍然存活时有意义（EXPLAIN/埋点用）。
#pragma once

#include <cstddef>
#include <expected>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "exec_defs.h"
#include "planner/plan.h"
#include "relation/cursor.h"
#include "relation/table.h"
#include "sql_types/cursor.h"
#include "sql_types/key_range.h"
#include "sql_types/key_set.h"
#include "sql_types/row.h"
#include "sql_types/schema.h"
#include "sql_types/value.h"

namespace exec {

// ============================================================
// 执行统计（EXPLAIN ANALYZE 用）
//
// ExecReport 由调用方（session）按"一次执行"创建一个，传给 creator/execute；
// 不传就完全不统计（算子里只有一个空指针判断）。
// ============================================================
struct NodeStats {
  const plan::PlanNode *node = nullptr;
  size_t rows = 0;      // 这个算子产出的行数
  int64_t start_us = 0; // 第一次 open 的时刻
  int64_t end_us = 0;   // 结束（END / close）的时刻；0 = 还没结束
};

class ExecReport {
public:
  void on_open(const plan::PlanNode *node);
  void on_row(const plan::PlanNode *node);
  void on_finish(const plan::PlanNode *node);

  const std::vector<NodeStats> &nodes() const { return nodes_; }
  const NodeStats *find(const plan::PlanNode *node) const;
  int64_t elapsed_us(const plan::PlanNode *node) const;

private:
  NodeStats &slot(const plan::PlanNode *node);
  std::vector<NodeStats> nodes_;
};

// 计划树 + 实际统计（report 为空时就是纯计划树）
std::string explain_text(const plan::PlanNode &root,
                         const ExecReport *report = nullptr);

// ============================================================
// 算子接口（框架内部 SPI）
// ============================================================
class Executor {
public:
  virtual ~Executor() = default;

  // 启动：定位迭代器、把需要物化的数据拉完（Sort）、执行写语句。
  // 重复 open 返回 ExecErrorCode::ALREADY_OPEN。
  ExecError open();

  // 取下一行：
  //   有值            -> Row
  //   CursorError::END -> 正常结束
  //   其它错误码       -> 出错（会粘住，和 sql::Cursor 的约定一致）
  std::expected<sql::Row, sql::CursorError> next();

  // 释放资源（迭代器/排序缓冲/物化结果）；幂等，析构也应调用
  void close();

  // 自己对应的计划节点（框架做 EXPLAIN/埋点用，不转发给客户端）
  virtual const plan::PlanNode *plan() const = 0;

  // 挂上统计器（factory 建好后调用；nullptr = 不统计）
  void set_report(ExecReport *report) { report_ = report; }

protected:
  virtual ExecError open_impl() = 0;
  virtual std::expected<sql::Row, sql::CursorError> next_impl() = 0;
  virtual void close_impl() = 0;

private:
  ExecReport *report_ = nullptr;

public:
  // 已经发生的行流错误（OK = 还没出错，END = 已经到末尾）。
  // open() 失败时上层用它拿"更精确的那个错误"（open 返回的是 ExecError）。
  virtual const sql::CursorError &error() const {
    static const sql::CursorError no_error{};
    return no_error;
  }

  // 是否产出结果行：SELECT 类为 true；INSERT/UPDATE/DELETE 为 false
  virtual bool produces_rows() const { return true; }

  // 是否是"写语句"（影响行数有意义）：INSERT/UPDATE/DELETE 为 true，
  // DDL/USE 的 EmptyExecutor 为 false —— 客户端用它区分
  // "OK, N rows affected" 与纯 "OK"
  virtual bool is_write() const { return false; }

  // 写语句的受影响行数（写完才有意义；非写算子恒为 0）
  virtual size_t affected_rows() const { return 0; }
};

// ============================================================
// ScanExecutor：扫 FullScan / IndexScan / RangeUnion
//
//   - 多区间按序拼接（降序时区间数组逆序 + 每个区间反向扫）；
//   - 点查询退化成一次 Get（pk 为 NULL 的点除外：走扫描）；
//   - exclude_keys 归并跳过（对应谓词仍在 Filter 里兜底）。
// ============================================================
class ScanExecutor : public Executor {
public:
  ScanExecutor(const plan::ScanPlan *plan, std::shared_ptr<sql::Table> table);
  ~ScanExecutor() override { close(); }

  ExecError open_impl() override;
  std::expected<sql::Row, sql::CursorError> next_impl() override;
  void close_impl() override;
  const plan::PlanNode *plan() const override { return plan_; }
  const sql::CursorError &error() const override { return error_; }

private:
  bool is_excluded(const sql::Value &primary_key) const;
  // 切到第 index 个区间；没有下一个区间时返回 false
  bool open_range(size_t index);

  const plan::ScanPlan *plan_;
  std::shared_ptr<sql::Table> table_;
  std::vector<sql::KeyRange> ranges_;
  sql::KeySet exclude_keys_;
  bool ascending_ = true;

  bool opened_ = false;
  bool done_ = false;
  sql::CursorError error_;
  size_t range_index_ = 0;                         // 当前扫到第几个区间
  std::unique_ptr<sql::TableCursor> range_cursor_; // 当前区间的游标
  // 点查询的退化路径（一次 Get）
  bool point_lookup_ = false;
  sql::Value point_key_;
  std::optional<sql::Row> point_row_;
};

// ============================================================
// FilterExecutor：谓词求值（三值逻辑，只放行 TRUE）
// ============================================================
class FilterExecutor : public Executor {
public:
  FilterExecutor(const plan::FilterPlan *plan, std::unique_ptr<Executor> child,
                 std::shared_ptr<sql::Table> table);
  ~FilterExecutor() override { close(); }

  ExecError open_impl() override;
  std::expected<sql::Row, sql::CursorError> next_impl() override;
  void close_impl() override;
  const plan::PlanNode *plan() const override { return plan_; }
  const sql::CursorError &error() const override { return child_->error(); }
  bool produces_rows() const override { return true; }

private:
  bool keeps(const sql::Row &row) const;

  const plan::FilterPlan *plan_;
  std::unique_ptr<Executor> child_;
  std::shared_ptr<sql::Table> table_;
  sql::ConditionPtr condition_; // 拷贝自计划节点（见文件头的生命周期说明）
};

// ============================================================
// SortExecutor：全量排序 / TopN
//
//   比较规则：按 order_by 逐列（NULL 最小），末尾追加主键做 tiebreaker，
//   否则并列时 LIMIT 取哪几行不确定。
//   内存：TopN 只留 n 行；全量排序超过 row_limit 报 MEMORY_LIMIT。
// ============================================================
class SortExecutor : public Executor {
public:
  SortExecutor(const plan::SortPlan *plan, std::unique_ptr<Executor> child,
               std::shared_ptr<sql::Table> table, size_t row_limit = 1'000'000);
  ~SortExecutor() override { close(); }

  ExecError open_impl() override;
  std::expected<sql::Row, sql::CursorError> next_impl() override;
  void close_impl() override;
  const plan::PlanNode *plan() const override { return plan_; }
  const sql::CursorError &error() const override { return error_; }
  bool produces_rows() const override { return true; }

private:
  const plan::SortPlan *plan_;
  std::unique_ptr<Executor> child_;
  std::shared_ptr<sql::Table> table_;
  size_t row_limit_;
  std::vector<sql::OrderByItem> order_by_;
  bool is_top_n_ = false;
  size_t top_n_ = 0;

  bool opened_ = false;
  bool done_ = false;
  size_t cursor_ = 0;
  sql::CursorError error_;
  std::vector<sql::Row> rows_; // TopN 时只保留 n 行
};

// ============================================================
// LimitExecutor：OFFSET / LIMIT（提前停止）
// ============================================================
class LimitExecutor : public Executor {
public:
  LimitExecutor(const plan::LimitPlan *plan, std::unique_ptr<Executor> child);
  ~LimitExecutor() override { close(); }

  ExecError open_impl() override;
  std::expected<sql::Row, sql::CursorError> next_impl() override;
  void close_impl() override;
  const plan::PlanNode *plan() const override { return plan_; }
  const sql::CursorError &error() const override { return child_->error(); }
  bool produces_rows() const override { return true; }

private:
  const plan::LimitPlan *plan_;
  std::unique_ptr<Executor> child_;
  sql::LimitClause limit_;
  size_t skipped_ = 0;
  size_t produced_ = 0;
  bool done_ = false;
};

// ============================================================
// ProjectExecutor：只保留需要的列（按 SELECT 列表的顺序）
// ============================================================
class ProjectExecutor : public Executor {
public:
  ProjectExecutor(const plan::ProjectPlan *plan,
                  std::unique_ptr<Executor> child,
                  std::shared_ptr<sql::Table> table);
  ~ProjectExecutor() override { close(); }

  ExecError open_impl() override;
  std::expected<sql::Row, sql::CursorError> next_impl() override;
  void close_impl() override;
  const plan::PlanNode *plan() const override { return plan_; }
  const sql::CursorError &error() const override { return child_->error(); }
  bool produces_rows() const override { return true; }

private:
  const plan::ProjectPlan *plan_;
  std::unique_ptr<Executor> child_;
  std::shared_ptr<sql::Table> table_;
  std::vector<sql::ColumnRef> columns_; // 拷贝自计划节点
  std::vector<int> column_indexes_;     // 空 = SELECT *
};

// ============================================================
// 写算子：open() 里把活干完（拉完子节点），next() 直接 END
//
//   UpdateExecutor：拉一行、改一行（列值来自语句的 SET 列表）
//   DeleteExecutor：拉一遍收集主键，再批量删（边扫边删会动到迭代器）
//   InsertExecutor：行源是语句里的 VALUES（没有子节点）
// ============================================================
class UpdateExecutor : public Executor {
public:
  UpdateExecutor(const plan::UpdatePlan *plan, std::unique_ptr<Executor> child,
                 std::shared_ptr<sql::Table> table);
  ~UpdateExecutor() override { close(); }

  ExecError open_impl() override;
  std::expected<sql::Row, sql::CursorError> next_impl() override {
    return end_row();
  }
  void close_impl() override;
  const plan::PlanNode *plan() const override { return plan_; }
  const sql::CursorError &error() const override { return error_; }
  bool produces_rows() const override { return false; }
  bool is_write() const override { return true; }
  size_t affected_rows() const override { return affected_rows_; }

private:
  std::expected<sql::Row, sql::CursorError> end_row();

  const plan::UpdatePlan *plan_;
  std::unique_ptr<Executor> child_;
  std::shared_ptr<sql::Table> table_;
  std::vector<sql::UpdateQuery::Assignment> assignments_; // SET 列表
  bool opened_ = false;
  size_t affected_rows_ = 0;
  sql::CursorError error_;
};

class DeleteExecutor : public Executor {
public:
  DeleteExecutor(const plan::DeletePlan *plan, std::unique_ptr<Executor> child,
                 std::shared_ptr<sql::Table> table);
  ~DeleteExecutor() override { close(); }

  ExecError open_impl() override;
  std::expected<sql::Row, sql::CursorError> next_impl() override {
    return end_row();
  }
  void close_impl() override;
  const plan::PlanNode *plan() const override { return plan_; }
  const sql::CursorError &error() const override { return error_; }
  bool produces_rows() const override { return false; }
  bool is_write() const override { return true; }
  size_t affected_rows() const override { return affected_rows_; }

private:
  std::expected<sql::Row, sql::CursorError> end_row();

  const plan::DeletePlan *plan_;
  std::unique_ptr<Executor> child_;
  std::shared_ptr<sql::Table> table_;
  bool opened_ = false;
  size_t affected_rows_ = 0;
  sql::CursorError error_;
};

class InsertExecutor : public Executor {
public:
  InsertExecutor(const plan::InsertPlan *plan,
                 std::shared_ptr<sql::Table> table);
  ~InsertExecutor() override { close(); }

  ExecError open_impl() override;
  std::expected<sql::Row, sql::CursorError> next_impl() override {
    return end_row();
  }
  void close_impl() override;
  const plan::PlanNode *plan() const override { return plan_; }
  const sql::CursorError &error() const override { return error_; }
  bool produces_rows() const override { return false; }
  bool is_write() const override { return true; }
  size_t affected_rows() const override { return affected_rows_; }

private:
  std::expected<sql::Row, sql::CursorError> end_row();
  // 把语句里的第 row 组值按 schema 排成一行（缺列 -> NULL）
  std::expected<sql::Row, sql::CursorError> build_row(size_t row) const;

  const plan::InsertPlan *plan_;
  std::shared_ptr<sql::Table> table_;
  std::vector<sql::Identifier> columns_;        // 列清单（可空 = 全部列）
  std::vector<std::vector<sql::Value>> values_; // VALUES
  bool opened_ = false;
  size_t affected_rows_ = 0;
  sql::CursorError error_;
};

// ============================================================
// 工厂：计划树 -> 算子树
//
// table 是计划里那张表的视图（调用方用 catalog.open_table 拿到）。
// DDL / USE 节点在这里没有对应算子：返回 ExecErrorCode::UNSUPPORTED_STMT。
// ============================================================
class ExecutorFactory {
public:
  static std::expected<std::unique_ptr<Executor>, ExecError>
  create(const plan::PlanNode &plan, std::shared_ptr<sql::Table> table,
         ExecReport *report = nullptr);
};

// ============================================================
// ResultCursor：客户端门面
//
//   - SELECT 类：**惰性** open（第一次 next() 才真正开始扫），
//     客户端拿到游标就不看了的话，不会白扫；
//   - 写语句：execute() 时就 open()（立即执行），游标只是为了统一返回类型；
//   - close() 幂等，析构兜底调用；错误从 next() / error() 都能拿到。
// ============================================================
class ResultCursor : public sql::Cursor {
public:
  ResultCursor(std::unique_ptr<Executor> root,
               std::shared_ptr<sql::Table> table,
               std::vector<std::string> columns = {});
  ~ResultCursor() override { close(); }

  // 门面实现的是 sql::Cursor（不是 Executor 的 SPI）
  std::expected<sql::Row, sql::CursorError> next() override;
  void close() override;

  // 立即启动根算子（行流语句也可以提前调用；重复调用幂等）。
  // 写语句在 execute() 里就是用这个把语句跑完的。
  ExecError open_now();

  size_t affected_rows() const;
  const Executor *root() const { return root_.get(); }
  // 结果列名（客户端格式化表格用）：SELECT 是投影列或表的所有列；
  // 写语句 / DDL 为空
  const std::vector<std::string> &columns() const { return columns_; }
  // 已经失败时的错误（OK 表示还没出错）
  const sql::CursorError &error() const { return error_; }

private:
  std::unique_ptr<Executor> root_;
  std::shared_ptr<sql::Table> table_;
  std::vector<std::string> columns_;
  bool opened_ = false;
  bool closed_ = false;
  sql::CursorError error_;
};

// ============================================================
// 入口：执行一条 DML 计划
//
//   行流语句（SELECT 类）：返回游标，第一次 next() 才启动根算子；
//   写语句：这里就把语句跑完（受影响行数从 affected_rows() 取），
//           游标随后只会返回 END。
// ============================================================
std::expected<std::unique_ptr<ResultCursor>, ExecError>
execute(const plan::PlanNode &plan, std::shared_ptr<sql::Table> table,
        ExecReport *report = nullptr);

// 一个"没有行流"的游标：DDL / USE 这类没有结果集的语句用（next() 直接 END）。
// 有它，"一条 SQL -> 一个游标"的调用约定就能统一。
std::unique_ptr<ResultCursor> empty_result();

} // namespace exec
