// executor.cpp
#include "executor.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <limits>
#include <string>
#include <utility>

#include "sql_types/sql_truth.h"

namespace exec {
namespace {

using sql::CursorError;
using sql::CursorErrorCode;

std::expected<sql::Row, CursorError> end_of_stream() {
  return std::unexpected(sql::end_of_stream());
}

// 行流错误 -> 执行器错误（open() 的返回通道）
ExecError to_exec_error(const CursorError &error) {
  switch (error.code) {
  case CursorErrorCode::IO_ERROR:
    return ExecError(ExecErrorCode::IO_ERROR, error.to_string());
  case CursorErrorCode::SCHEMA_ERROR:
    return ExecError(ExecErrorCode::SCHEMA_ERROR, error.to_string());
  case CursorErrorCode::NOT_FOUND:
    return ExecError(ExecErrorCode::TABLE_NOT_FOUND, error.to_string());
  case CursorErrorCode::INVALID_ARGUMENT:
    return ExecError(ExecErrorCode::INVALID_ARGUMENT, error.to_string());
  default:
    return ExecError(ExecErrorCode::INTERNAL, error.to_string());
  }
}

// 排序语义下的比较：NULL 最小（MySQL：ASC 时 NULL 在前）。
// 注意不能直接用 sql_order() 的结果 —— 它对 NULL 返回 nullopt（三值逻辑），
// 而排序需要一个全序；跨族不可比时用 key 编码兜底，保证顺序确定。
int compare_for_sort(const sql::Value &a, const sql::Value &b) {
  if (a.is_null() || b.is_null()) {
    if (a.is_null() && b.is_null()) {
      return 0;
    }
    return a.is_null() ? -1 : 1;
  }
  if (auto order = sql::sql_order(a, b)) {
    return *order;
  }
  const sql::Key ka = a.to_key();
  const sql::Key kb = b.to_key();
  if (ka == kb) {
    return 0;
  }
  return ka < kb ? -1 : 1;
}

// 排序键：列下标 + 方向
struct SortKey {
  int index = -1;
  bool ascending = true;
};

// 行比较器：按 order_by 逐列比较，末尾追加主键做 tiebreaker
class RowComparator {
public:
  RowComparator(const sql::TableSchema &schema,
                const std::vector<sql::OrderByItem> &order_by) {
    for (const auto &item : order_by) {
      const int index = schema.column_index(item.column);
      if (index < 0) {
        error_ = ExecError(ExecErrorCode::COLUMN_NOT_FOUND,
                           "order by column not found: " + item.column.str());
        return;
      }
      keys_.push_back(
          SortKey{index, item.direction == sql::OrderDirection::ASC});
    }
    // tiebreaker：并列时用主键定序，否则 LIMIT 取哪几行不确定
    const int pk = schema.primary_key_index();
    if (pk >= 0 && (keys_.empty() || keys_.back().index != pk)) {
      keys_.push_back(SortKey{pk, true});
    }
  }

  bool valid() const { return error_.ok(); }
  const ExecError &error() const { return error_; }

  // a 应该排在 b 前面
  bool less(const sql::Row &a, const sql::Row &b) const {
    for (const auto &key : keys_) {
      const size_t index = static_cast<size_t>(key.index);
      if (index >= a.size() || index >= b.size()) {
        continue;
      }
      const int cmp = compare_for_sort(a[index], b[index]);
      if (cmp == 0) {
        continue;
      }
      return key.ascending ? cmp < 0 : cmp > 0;
    }
    return false; // 完全相等：保持原有相对顺序
  }

private:
  std::vector<SortKey> keys_;
  ExecError error_;
};

} // namespace

// ============================================================
// 执行统计（EXPLAIN ANALYZE）
// ============================================================
namespace {

int64_t now_micros() {
  return std::chrono::duration_cast<std::chrono::microseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

// 微秒 -> "0.123ms" / "12.3us"
std::string format_micros(int64_t micros) {
  if (micros < 0) {
    return "-";
  }
  if (micros < 1000) {
    return std::to_string(micros) + "us";
  }
  char buffer[32];
  std::snprintf(buffer, sizeof(buffer), "%.3fms",
                static_cast<double>(micros) / 1000.0);
  return buffer;
}

} // namespace

NodeStats &ExecReport::slot(const plan::PlanNode *node) {
  for (NodeStats &stats : nodes_) {
    if (stats.node == node) {
      return stats;
    }
  }
  nodes_.push_back(NodeStats{node, 0, 0, 0});
  return nodes_.back();
}

void ExecReport::on_open(const plan::PlanNode *node) {
  NodeStats &stats = slot(node);
  if (stats.start_us == 0) {
    stats.start_us = now_micros();
  }
}

void ExecReport::on_row(const plan::PlanNode *node) { ++slot(node).rows; }

void ExecReport::on_finish(const plan::PlanNode *node) {
  NodeStats &stats = slot(node);
  if (stats.end_us == 0) {
    stats.end_us = now_micros();
  }
}

const NodeStats *ExecReport::find(const plan::PlanNode *node) const {
  for (const NodeStats &stats : nodes_) {
    if (stats.node == node) {
      return &stats;
    }
  }
  return nullptr;
}

int64_t ExecReport::elapsed_us(const plan::PlanNode *node) const {
  const NodeStats *stats = find(node);
  if (stats == nullptr || stats->start_us == 0 || stats->end_us == 0) {
    return -1; // 没测到（比如根本没打开过）
  }
  return stats->end_us - stats->start_us;
}

std::string explain_text(const plan::PlanNode &root, const ExecReport *report) {
  std::string text;
  int depth = 0;
  for (const plan::PlanNode *node = &root; node != nullptr;
       node = node->child()) {
    text.append(static_cast<size_t>(depth) * 2, ' ');
    text += node->to_string();
    if (report != nullptr) {
      const NodeStats *stats = report->find(node);
      text += "  [rows=" + std::to_string(stats != nullptr ? stats->rows : 0);
      text += " time=" + format_micros(report->elapsed_us(node));
      text += " cost=" + node->cost().to_string() + "]";
    }
    text += "\n";
    ++depth;
  }
  return text;
}

// ============================================================
// NVI 包装：数行数/记时间只写一处，算子只管干活
// ============================================================
ExecError Executor::open() {
  if (report_ != nullptr) {
    report_->on_open(plan());
  }
  return open_impl();
}

std::expected<sql::Row, CursorError> Executor::next() {
  auto row = next_impl();
  if (report_ != nullptr) {
    if (row.has_value()) {
      report_->on_row(plan());
    } else {
      report_->on_finish(plan());
    }
  }
  return row;
}

void Executor::close() {
  close_impl();
  if (report_ != nullptr) {
    report_->on_finish(plan());
  }
}

// 空算子：没有任何行源，next() 直接 END（DDL / USE 用）
namespace {

class EmptyExecutor : public Executor {
public:
  explicit EmptyExecutor(const plan::PlanNode *plan) : plan_(plan) {}
  ~EmptyExecutor() override { close(); }

  ExecError open_impl() override { return ExecError(); }
  std::expected<sql::Row, CursorError> next_impl() override {
    error_ = sql::end_of_stream();
    return std::unexpected(error_);
  }
  void close_impl() override {
    if (!error_.is_error()) {
      error_ = sql::end_of_stream();
    }
  }
  const plan::PlanNode *plan() const override { return plan_; }
  const sql::CursorError &error() const override { return error_; }
  bool produces_rows() const override { return false; }

private:
  const plan::PlanNode *plan_ = nullptr;
  sql::CursorError error_;
};

} // namespace

namespace {

// 结果列名：投影节点决定顺序/别名；没有投影（SELECT *）就用表的所有列
std::vector<std::string> result_columns(const plan::PlanNode &root,
                                        const sql::Table &table) {
  for (const plan::PlanNode *node = &root; node != nullptr;
       node = node->child()) {
    const auto *project = dynamic_cast<const plan::ProjectPlan *>(node);
    if (project == nullptr) {
      continue;
    }
    const std::vector<sql::ColumnRef> &columns = project->columns();
    const bool wildcard =
        columns.empty() || (columns.size() == 1 && columns[0].is_wildcard());
    if (wildcard) {
      break;
    }
    std::vector<std::string> names;
    names.reserve(columns.size());
    for (const auto &column : columns) {
      names.push_back(column.display_name());
    }
    return names;
  }
  std::vector<std::string> names;
  for (const auto &column : table.schema().columns()) {
    names.push_back(column.name.str());
  }
  return names;
}

} // namespace

// ============================================================
// ScanExecutor
// ============================================================
ScanExecutor::ScanExecutor(const plan::ScanPlan *plan,
                           std::shared_ptr<sql::Table> table)
    : plan_(plan), table_(std::move(table)) {
  ascending_ = plan_->ascending();
  if (const auto *index = dynamic_cast<const plan::IndexScanPlan *>(plan_)) {
    ranges_.push_back(index->range());
  } else if (const auto *united =
                 dynamic_cast<const plan::RangeUnionPlan *>(plan_)) {
    ranges_ = united->ranges();
    exclude_keys_ = united->exclude_keys();
  } else {
    // FullScan：整张表（族范围由主键类型决定）
    ranges_.push_back(sql::KeyRange::all(plan_->target().primary_key_type));
  }
}

bool ScanExecutor::is_excluded(const sql::Value &primary_key) const {
  return !exclude_keys_.empty() && exclude_keys_.contains(primary_key);
}

bool ScanExecutor::open_range(size_t index) {
  for (size_t i = index; i < ranges_.size(); ++i) {
    // 降序：区间数组逆序 + 每个区间反向扫
    const size_t slot = ascending_ ? i : ranges_.size() - 1 - i;
    range_index_ = i;
    if (ranges_[slot].is_empty()) {
      continue;
    }
    range_cursor_ = table_->scan(ranges_[slot], ascending_);
    return true;
  }
  range_index_ = ranges_.size();
  range_cursor_.reset();
  return false;
}

ExecError ScanExecutor::open_impl() {
  if (opened_) {
    return ExecError(ExecErrorCode::ALREADY_OPEN, "scan already open");
  }
  opened_ = true;
  done_ = false;
  error_ = sql::CursorError();
  range_index_ = 0;
  range_cursor_.reset();
  point_row_.reset();
  point_lookup_ = false;

  if (table_ == nullptr) {
    return ExecError(ExecErrorCode::INVALID_ARGUMENT, "scan without table");
  }

  // 点查询退化成一次 Get（NULL 点例外：Table::find 不接受 NULL 键）
  if (ranges_.size() == 1 && ranges_[0].is_point()) {
    const sql::Value key = ranges_[0].point_value();
    if (!key.is_null() && !is_excluded(key)) {
      point_lookup_ = true;
      point_key_ = key;
    }
  }
  if (point_lookup_) {
    auto found = table_->find(point_key_);
    if (!found.has_value()) {
      error_ = sql::to_cursor_error(found.error());
      return to_exec_error(error_);
    }
    if (found->has_value()) {
      point_row_ = std::move(**found);
    }
    return ExecError();
  }

  if (!open_range(0)) {
    done_ = true;
  }
  return ExecError();
}

std::expected<sql::Row, CursorError> ScanExecutor::next_impl() {
  if (error_.is_error()) {
    return std::unexpected(error_);
  }
  if (done_) {
    error_ = sql::end_of_stream();
    return std::unexpected(error_);
  }

  if (point_lookup_) {
    if (point_row_.has_value()) {
      sql::Row row = std::move(*point_row_);
      point_row_.reset();
      return row;
    }
    error_ = sql::end_of_stream();
    return std::unexpected(error_);
  }

  while (true) {
    if (range_cursor_ == nullptr) {
      error_ = sql::end_of_stream();
      return std::unexpected(error_);
    }
    auto row = range_cursor_->next();
    if (row.has_value()) {
      if (is_excluded(table_->primary_key_of(*row))) {
        continue; // 跳点：少扫几行（正确性由 Filter 兜底）
      }
      return std::move(*row);
    }
    if (row.error().end()) {
      // 当前区间扫完 -> 切下一个区间（离散区间在这里拼成连续行流）
      if (!open_range(range_index_ + 1)) {
        error_ = sql::end_of_stream();
        return std::unexpected(error_);
      }
      continue;
    }
    error_ = row.error();
    return std::unexpected(error_);
  }
}

void ScanExecutor::close_impl() {
  if (range_cursor_ != nullptr) {
    range_cursor_->close();
    range_cursor_.reset();
  }
  point_row_.reset();
  if (!error_.is_error()) {
    error_ = sql::end_of_stream();
  }
  done_ = true;
}

// ============================================================
// FilterExecutor
// ============================================================
FilterExecutor::FilterExecutor(const plan::FilterPlan *plan,
                               std::unique_ptr<Executor> child,
                               std::shared_ptr<sql::Table> table)
    : plan_(plan), child_(std::move(child)), table_(std::move(table)) {
  // 拷贝谓词：运行期不再解引用计划节点（计划树可能已经释放）
  condition_ =
      plan->condition() != nullptr ? plan->condition()->clone() : nullptr;
}

bool FilterExecutor::keeps(const sql::Row &row) const {
  if (condition_ == nullptr) {
    return true;
  }
  const sql::TableSchema &schema = table_->schema();
  const sql::ValueLookup lookup =
      [&schema, &row](const sql::Identifier &column) -> const sql::Value * {
    const int index = schema.column_index(column);
    if (index < 0 || static_cast<size_t>(index) >= row.size()) {
      return nullptr; // 未知列 -> UNKNOWN
    }
    return &row[static_cast<size_t>(index)];
  };
  return sql::where_keeps(sql::evaluate_condition(*condition_, lookup));
}

ExecError FilterExecutor::open_impl() { return child_->open(); }

std::expected<sql::Row, CursorError> FilterExecutor::next_impl() {
  while (true) {
    auto row = child_->next();
    if (!row.has_value()) {
      return std::unexpected(row.error()); // END 或错误都原样透传
    }
    if (keeps(*row)) {
      return std::move(*row);
    }
  }
}

void FilterExecutor::close_impl() { child_->close(); }

// ============================================================
// SortExecutor
// ============================================================
SortExecutor::SortExecutor(const plan::SortPlan *plan,
                           std::unique_ptr<Executor> child,
                           std::shared_ptr<sql::Table> table, size_t row_limit)
    : plan_(plan), child_(std::move(child)), table_(std::move(table)),
      row_limit_(row_limit), order_by_(plan->order_by()),
      is_top_n_(plan->is_top_n()), top_n_(plan->top_n()) {}

ExecError SortExecutor::open_impl() {
  if (opened_) {
    return ExecError(ExecErrorCode::ALREADY_OPEN, "sort already open");
  }
  opened_ = true;
  rows_.clear();
  cursor_ = 0;
  done_ = false;
  error_ = sql::CursorError();

  ExecError child_status = child_->open();
  if (!child_status.ok()) {
    return child_status;
  }

  const RowComparator compare(table_->schema(), order_by_);
  if (!compare.valid()) {
    return compare.error();
  }
  const auto less = [&compare](const sql::Row &a, const sql::Row &b) {
    return compare.less(a, b);
  };

  // TopN：堆里只留 n 行（堆顶是"最差"的那行）；否则全量收集
  const bool top_n = is_top_n_;
  const size_t keep = top_n ? top_n_ : row_limit_;
  while (true) {
    auto row = child_->next();
    if (!row.has_value()) {
      if (row.error().end()) {
        break;
      }
      error_ = row.error();
      return to_exec_error(error_);
    }
    if (!top_n) {
      if (rows_.size() >= row_limit_) {
        error_ = CursorError(CursorErrorCode::INTERNAL, "sort memory limit");
        return ExecError(ExecErrorCode::MEMORY_LIMIT,
                         "sort buffer exceeded " + std::to_string(row_limit_) +
                             " rows (external sort not implemented yet)");
      }
      rows_.push_back(std::move(*row));
      continue;
    }
    if (keep == 0) {
      continue; // LIMIT 0
    }
    if (rows_.size() < keep) {
      rows_.push_back(std::move(*row));
      std::push_heap(rows_.begin(), rows_.end(), less);
      continue;
    }
    if (compare.less(*row, rows_.front())) { // 比堆顶（最差）更好 -> 换掉
      std::pop_heap(rows_.begin(), rows_.end(), less);
      rows_.back() = std::move(*row);
      std::push_heap(rows_.begin(), rows_.end(), less);
    }
  }
  // 堆排序 + 最终排序都用同一个比较器
  std::sort(rows_.begin(), rows_.end(), less);
  return ExecError();
}

std::expected<sql::Row, CursorError> SortExecutor::next_impl() {
  if (error_.is_error()) {
    return std::unexpected(error_);
  }
  if (done_) {
    error_ = sql::end_of_stream();
    return std::unexpected(error_);
  }
  if (cursor_ >= rows_.size()) {
    done_ = true;
    error_ = sql::end_of_stream();
    return std::unexpected(error_);
  }
  return std::move(rows_[cursor_++]);
}

void SortExecutor::close_impl() {
  if (child_ != nullptr) {
    child_->close();
  }
  rows_.clear();
  rows_.shrink_to_fit();
  done_ = true;
  if (!error_.is_error()) {
    error_ = sql::end_of_stream();
  }
}

// ============================================================
// LimitExecutor
// ============================================================
LimitExecutor::LimitExecutor(const plan::LimitPlan *plan,
                             std::unique_ptr<Executor> child)
    : plan_(plan), child_(std::move(child)), limit_(plan->limit()) {}

ExecError LimitExecutor::open_impl() {
  skipped_ = 0;
  produced_ = 0;
  done_ = false;
  return child_->open();
}

std::expected<sql::Row, CursorError> LimitExecutor::next_impl() {
  if (done_) {
    return end_of_stream();
  }
  const size_t offset = limit_.has_offset() ? limit_.offset_value() : 0;
  const size_t want = limit_.has_limit() ? limit_.limit_value()
                                         : std::numeric_limits<size_t>::max();

  while (skipped_ < offset) {
    auto skipped_row = child_->next();
    if (!skipped_row.has_value()) {
      done_ = true;
      return std::unexpected(skipped_row.error());
    }
    ++skipped_;
  }
  if (produced_ >= want) {
    done_ = true;
    return end_of_stream(); // LIMIT 到了：不再向上游要数据（早停）
  }
  auto row = child_->next();
  if (!row.has_value()) {
    done_ = true;
    return std::unexpected(row.error());
  }
  ++produced_;
  return std::move(*row);
}

void LimitExecutor::close_impl() {
  if (child_ != nullptr) {
    child_->close();
  }
  done_ = true;
}

// ============================================================
// ProjectExecutor
// ============================================================
ProjectExecutor::ProjectExecutor(const plan::ProjectPlan *plan,
                                 std::unique_ptr<Executor> child,
                                 std::shared_ptr<sql::Table> table)
    : plan_(plan), child_(std::move(child)), table_(std::move(table)),
      columns_(plan->columns()) {}

ExecError ProjectExecutor::open_impl() {
  column_indexes_.clear();
  const bool wildcard =
      columns_.empty() || (columns_.size() == 1 && columns_[0].is_wildcard());
  if (!wildcard) {
    for (const auto &column : columns_) {
      const int index = table_->schema().column_index(column.column);
      if (index < 0) {
        return ExecError(ExecErrorCode::COLUMN_NOT_FOUND,
                         "column not found: " + column.column.str());
      }
      column_indexes_.push_back(index);
    }
  }
  return child_->open();
}

std::expected<sql::Row, CursorError> ProjectExecutor::next_impl() {
  auto row = child_->next();
  if (!row.has_value()) {
    return std::unexpected(row.error());
  }
  if (column_indexes_.empty()) {
    return std::move(*row); // SELECT *
  }
  sql::Row projected;
  projected.reserve(column_indexes_.size());
  for (const int index : column_indexes_) {
    if (index >= 0 && static_cast<size_t>(index) < row->size()) {
      projected.push_back((*row)[static_cast<size_t>(index)]);
    }
  }
  return projected;
}

void ProjectExecutor::close_impl() {
  if (child_ != nullptr) {
    child_->close();
  }
}

// ============================================================
// 写算子
// ============================================================
UpdateExecutor::UpdateExecutor(const plan::UpdatePlan *plan,
                               std::unique_ptr<Executor> child,
                               std::shared_ptr<sql::Table> table)
    : plan_(plan), child_(std::move(child)), table_(std::move(table)) {
  const sql::UpdateQuery *update = plan->query().query.update();
  if (update != nullptr) {
    assignments_ = update->assignments; // 拷贝 SET 列表
  }
}

ExecError UpdateExecutor::open_impl() {
  if (opened_) {
    return ExecError(ExecErrorCode::ALREADY_OPEN, "update already open");
  }
  opened_ = true;
  affected_rows_ = 0;
  error_ = sql::CursorError();

  ExecError child_status = child_->open();
  if (!child_status.ok()) {
    return child_status;
  }

  while (true) {
    auto row = child_->next();
    if (!row.has_value()) {
      if (row.error().end()) {
        break;
      }
      error_ = row.error();
      return to_exec_error(error_);
    }
    const sql::Value primary_key = table_->primary_key_of(*row);
    if (primary_key.is_null()) {
      error_ = CursorError(CursorErrorCode::INVALID_ARGUMENT,
                           "row has NULL primary key");
      return to_exec_error(error_);
    }
    sql::Row updated = *row;
    for (const auto &assignment : assignments_) {
      const int index = table_->schema().column_index(assignment.column);
      if (index < 0) {
        error_ = CursorError(CursorErrorCode::INVALID_ARGUMENT,
                             "column not found: " + assignment.column.str());
        return to_exec_error(error_);
      }
      updated[static_cast<size_t>(index)] = assignment.value;
    }
    auto applied = table_->update(primary_key, updated);
    if (!applied.has_value()) {
      error_ = sql::to_cursor_error(applied.error());
      return to_exec_error(error_);
    }
    ++affected_rows_;
  }
  return ExecError();
}

std::expected<sql::Row, CursorError> UpdateExecutor::end_row() {
  if (error_.is_error()) {
    return std::unexpected(error_);
  }
  error_ = sql::end_of_stream();
  return std::unexpected(error_);
}

void UpdateExecutor::close_impl() {
  if (child_ != nullptr) {
    child_->close();
  }
  if (!error_.is_error()) {
    error_ = sql::end_of_stream();
  }
}

DeleteExecutor::DeleteExecutor(const plan::DeletePlan *plan,
                               std::unique_ptr<Executor> child,
                               std::shared_ptr<sql::Table> table)
    : plan_(plan), child_(std::move(child)), table_(std::move(table)) {}

ExecError DeleteExecutor::open_impl() {
  if (opened_) {
    return ExecError(ExecErrorCode::ALREADY_OPEN, "delete already open");
  }
  opened_ = true;
  affected_rows_ = 0;
  error_ = sql::CursorError();

  ExecError child_status = child_->open();
  if (!child_status.ok()) {
    return child_status;
  }

  // 先把要删的主键收集出来，再统一删：边扫边删会动到迭代器状态
  std::vector<sql::Value> doomed;
  while (true) {
    auto row = child_->next();
    if (!row.has_value()) {
      if (row.error().end()) {
        break;
      }
      error_ = row.error();
      return to_exec_error(error_);
    }
    const sql::Value primary_key = table_->primary_key_of(*row);
    if (primary_key.is_null()) {
      error_ = CursorError(CursorErrorCode::INVALID_ARGUMENT,
                           "row has NULL primary key");
      return to_exec_error(error_);
    }
    doomed.push_back(primary_key);
  }
  for (const auto &primary_key : doomed) {
    auto removed = table_->remove(primary_key);
    if (!removed.has_value()) {
      error_ = sql::to_cursor_error(removed.error());
      return to_exec_error(error_);
    }
    ++affected_rows_;
  }
  return ExecError();
}

std::expected<sql::Row, CursorError> DeleteExecutor::end_row() {
  if (error_.is_error()) {
    return std::unexpected(error_);
  }
  error_ = sql::end_of_stream();
  return std::unexpected(error_);
}

void DeleteExecutor::close_impl() {
  if (child_ != nullptr) {
    child_->close();
  }
  if (!error_.is_error()) {
    error_ = sql::end_of_stream();
  }
}

InsertExecutor::InsertExecutor(const plan::InsertPlan *plan,
                               std::shared_ptr<sql::Table> table)
    : plan_(plan), table_(std::move(table)) {
  const sql::InsertQuery *insert = plan->query().query.insert();
  if (insert != nullptr) {
    columns_ = insert->columns; // 拷贝列清单与 VALUES：运行期不再看计划
    values_ = insert->values;
  }
}

std::expected<sql::Row, CursorError>
InsertExecutor::build_row(size_t row_index) const {
  if (row_index >= values_.size()) {
    return std::unexpected(CursorError(CursorErrorCode::INVALID_ARGUMENT,
                                       "insert row index out of range"));
  }
  const sql::TableSchema &schema = table_->schema();
  const std::vector<sql::Value> &values = values_[row_index];

  // 缺省先填 NULL：没有出现在列清单里的列就是 NULL（NOT NULL 列会在
  // validate_row 里被拒，这正是我们要的语义）
  sql::Row row;
  row.reserve(schema.column_count());
  for (size_t i = 0; i < schema.column_count(); ++i) {
    row.push_back(sql::Value());
  }

  if (columns_.empty()) {
    if (values.size() != schema.column_count()) {
      return std::unexpected(
          CursorError(CursorErrorCode::INVALID_ARGUMENT,
                      "column count mismatch: expected " +
                          std::to_string(schema.column_count()) + ", got " +
                          std::to_string(values.size())));
    }
    for (size_t i = 0; i < values.size(); ++i) {
      row[i] = values[i];
    }
    return row;
  }

  if (values.size() != columns_.size()) {
    return std::unexpected(CursorError(
        CursorErrorCode::INVALID_ARGUMENT,
        "value count mismatch: expected " + std::to_string(columns_.size()) +
            ", got " + std::to_string(values.size())));
  }
  for (size_t i = 0; i < values.size(); ++i) {
    const int index = schema.column_index(columns_[i]);
    if (index < 0) {
      return std::unexpected(
          CursorError(CursorErrorCode::INVALID_ARGUMENT,
                      "column not found: " + columns_[i].str()));
    }
    row[static_cast<size_t>(index)] = values[i];
  }
  return row;
}

ExecError InsertExecutor::open_impl() {
  if (opened_) {
    return ExecError(ExecErrorCode::ALREADY_OPEN, "insert already open");
  }
  opened_ = true;
  affected_rows_ = 0;
  error_ = sql::CursorError();

  for (size_t i = 0; i < values_.size(); ++i) {
    auto row = build_row(i);
    if (!row.has_value()) {
      error_ = row.error();
      return to_exec_error(error_);
    }
    auto inserted = table_->insert(*row);
    if (!inserted.has_value()) {
      error_ = sql::to_cursor_error(inserted.error());
      return to_exec_error(error_);
    }
    ++affected_rows_;
  }
  return ExecError();
}

std::expected<sql::Row, CursorError> InsertExecutor::end_row() {
  if (error_.is_error()) {
    return std::unexpected(error_);
  }
  error_ = sql::end_of_stream();
  return std::unexpected(error_);
}

void InsertExecutor::close_impl() {
  if (!error_.is_error()) {
    error_ = sql::end_of_stream();
  }
}

// ============================================================
// 工厂
// ============================================================
std::expected<std::unique_ptr<Executor>, ExecError>
ExecutorFactory::create(const plan::PlanNode &plan,
                        std::shared_ptr<sql::Table> table, ExecReport *report) {
  if (table == nullptr) {
    return std::unexpected(ExecError(ExecErrorCode::INVALID_ARGUMENT,
                                     "create executor without table"));
  }
  // 统计器挂到每个算子上（nullptr = 不统计）
  const auto attach = [report](std::unique_ptr<Executor> executor) {
    if (executor != nullptr && report != nullptr) {
      executor->set_report(report);
    }
    return executor;
  };

  switch (plan.type()) {
  case plan::PlanType::FULL_SCAN:
  case plan::PlanType::INDEX_SCAN:
  case plan::PlanType::RANGE_UNION: {
    const auto *scan = dynamic_cast<const plan::ScanPlan *>(&plan);
    if (scan == nullptr) {
      return std::unexpected(ExecError(ExecErrorCode::INVALID_ARGUMENT,
                                       "node is not a scan plan"));
    }
    if (scan->target().table != table->table_name()) {
      return std::unexpected(
          ExecError(ExecErrorCode::INVALID_ARGUMENT,
                    "plan targets table '" + scan->target().table.str() +
                        "' but got '" + table->table_name().str() + "'"));
    }
    return attach(std::make_unique<ScanExecutor>(scan, std::move(table)));
  }
  case plan::PlanType::FILTER: {
    const auto *filter = dynamic_cast<const plan::FilterPlan *>(&plan);
    if (filter == nullptr || filter->child() == nullptr) {
      return std::unexpected(
          ExecError(ExecErrorCode::INVALID_ARGUMENT, "filter without child"));
    }
    auto child = create(*filter->child(), table, report);
    if (!child.has_value()) {
      return std::unexpected(child.error());
    }
    return attach(std::make_unique<FilterExecutor>(filter, std::move(*child),
                                                   std::move(table)));
  }
  case plan::PlanType::SORT: {
    const auto *sort = dynamic_cast<const plan::SortPlan *>(&plan);
    if (sort == nullptr || sort->child() == nullptr) {
      return std::unexpected(
          ExecError(ExecErrorCode::INVALID_ARGUMENT, "sort without child"));
    }
    auto child = create(*sort->child(), table, report);
    if (!child.has_value()) {
      return std::unexpected(child.error());
    }
    return attach(std::make_unique<SortExecutor>(sort, std::move(*child),
                                                 std::move(table)));
  }
  case plan::PlanType::LIMIT: {
    const auto *limit = dynamic_cast<const plan::LimitPlan *>(&plan);
    if (limit == nullptr || limit->child() == nullptr) {
      return std::unexpected(
          ExecError(ExecErrorCode::INVALID_ARGUMENT, "limit without child"));
    }
    auto child = create(*limit->child(), table, report);
    if (!child.has_value()) {
      return std::unexpected(child.error());
    }
    return attach(std::make_unique<LimitExecutor>(limit, std::move(*child)));
  }
  case plan::PlanType::PROJECT: {
    const auto *project = dynamic_cast<const plan::ProjectPlan *>(&plan);
    if (project == nullptr || project->child() == nullptr) {
      return std::unexpected(
          ExecError(ExecErrorCode::INVALID_ARGUMENT, "project without child"));
    }
    auto child = create(*project->child(), table, report);
    if (!child.has_value()) {
      return std::unexpected(child.error());
    }
    return attach(std::make_unique<ProjectExecutor>(project, std::move(*child),
                                                    std::move(table)));
  }
  case plan::PlanType::UPDATE: {
    const auto *update = dynamic_cast<const plan::UpdatePlan *>(&plan);
    if (update == nullptr || update->child() == nullptr) {
      return std::unexpected(
          ExecError(ExecErrorCode::INVALID_ARGUMENT, "update without child"));
    }
    auto child = create(*update->child(), table, report);
    if (!child.has_value()) {
      return std::unexpected(child.error());
    }
    return attach(std::make_unique<UpdateExecutor>(update, std::move(*child),
                                                   std::move(table)));
  }
  case plan::PlanType::DELETE: {
    const auto *del = dynamic_cast<const plan::DeletePlan *>(&plan);
    if (del == nullptr || del->child() == nullptr) {
      return std::unexpected(
          ExecError(ExecErrorCode::INVALID_ARGUMENT, "delete without child"));
    }
    auto child = create(*del->child(), table, report);
    if (!child.has_value()) {
      return std::unexpected(child.error());
    }
    return attach(std::make_unique<DeleteExecutor>(del, std::move(*child),
                                                   std::move(table)));
  }
  case plan::PlanType::INSERT: {
    const auto *insert = dynamic_cast<const plan::InsertPlan *>(&plan);
    if (insert == nullptr) {
      return std::unexpected(ExecError(ExecErrorCode::INVALID_ARGUMENT,
                                       "node is not an insert plan"));
    }
    return attach(std::make_unique<InsertExecutor>(insert, std::move(table)));
  }
  default:
    // DDL / USE 没有行流：由上层用 sql::Catalog 执行（USE 还需要会话状态）
    return std::unexpected(
        ExecError(ExecErrorCode::UNSUPPORTED_STMT,
                  std::string("executor does not run ") +
                      plan::plan_type_to_string(plan.type()) + " statements"));
  }
}

// ============================================================
// ResultCursor：客户端门面
// ============================================================
ResultCursor::ResultCursor(std::unique_ptr<Executor> root,
                           std::shared_ptr<sql::Table> table,
                           std::vector<std::string> columns)
    : root_(std::move(root)), table_(std::move(table)),
      columns_(std::move(columns)) {}

ExecError ResultCursor::open_now() {
  if (closed_) {
    error_ = sql::end_of_stream();
    return ExecError(ExecErrorCode::NOT_OPEN, "cursor is closed");
  }
  if (opened_) {
    return ExecError();
  }
  opened_ = true;
  ExecError status = root_->open();
  if (!status.ok()) {
    // open 内部的失败往往有更精确的行流错误（比如 SCHEMA_ERROR）
    error_ = root_->error().is_error()
                 ? root_->error()
                 : CursorError(CursorErrorCode::INTERNAL, status.to_string());
  }
  return status;
}

std::expected<sql::Row, CursorError> ResultCursor::next() {
  if (error_.is_error()) {
    return std::unexpected(error_);
  }
  if (closed_) {
    error_ = sql::end_of_stream();
    return std::unexpected(error_);
  }
  if (!opened_) {
    ExecError status = open_now();
    if (!status.ok()) {
      return std::unexpected(error_);
    }
  }
  auto row = root_->next();
  if (!row.has_value()) {
    error_ = row.error();
  }
  return row;
}

void ResultCursor::close() {
  if (closed_) {
    return;
  }
  closed_ = true;
  if (root_ != nullptr) {
    root_->close();
  }
  if (!error_.is_error()) {
    error_ = sql::end_of_stream();
  }
}

size_t ResultCursor::affected_rows() const {
  return root_ != nullptr ? root_->affected_rows() : 0;
}

std::expected<std::unique_ptr<ResultCursor>, ExecError>
execute(const plan::PlanNode &plan, std::shared_ptr<sql::Table> table,
        ExecReport *report) {
  auto root = ExecutorFactory::create(plan, table, report);
  if (!root.has_value()) {
    return std::unexpected(root.error());
  }
  // 结果列名：有投影节点就用投影列，否则用表的所有列（客户端格式化用）
  std::vector<std::string> columns;
  if ((*root)->produces_rows() && table != nullptr) {
    columns = result_columns(plan, *table);
  }
  auto cursor = std::make_unique<ResultCursor>(
      std::move(*root), std::move(table), std::move(columns));

  // 写语句：这里就执行（客户端不会来 fetch，语句必须真的落地）。
  // 行流语句（SELECT 类）：惰性 open，第一次 next() 才启动。
  if (!cursor->root()->produces_rows()) {
    ExecError status = cursor->open_now();
    if (!status.ok()) {
      return std::unexpected(status);
    }
  }
  return cursor;
}

std::unique_ptr<ResultCursor> empty_result() {
  return std::make_unique<ResultCursor>(
      std::make_unique<EmptyExecutor>(nullptr), nullptr);
}

} // namespace exec
