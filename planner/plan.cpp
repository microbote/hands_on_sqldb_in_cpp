// plan.cpp
//
// 计划节点的文本形式：只用于调试 / 日志 / EXPLAIN。
// 输出格式刻意保持稳定（测试会断言关键片段），但**不**参与任何语义判断。
#include "plan.h"

#include <string>

namespace plan {
namespace {

std::string ranges_to_string(const std::vector<sql::KeyRange> &ranges) {
  std::string s = "[";
  for (size_t i = 0; i < ranges.size(); ++i) {
    if (i > 0) {
      s += ", ";
    }
    s += ranges[i].to_string();
  }
  s += "]";
  return s;
}

std::string writes_to_string(const OptimizedQuery &query) {
  std::string s = query.table.empty() ? "?" : query.table.str();
  if (!query.primary_key.empty()) {
    s += " pk=" + query.primary_key.str();
  }
  if (query.remaining_filter) {
    s += " filter=" + query.remaining_filter->to_string();
  }
  return s;
}

} // namespace

std::string FullScan::to_string() const {
  return "FullScan(" + target_.to_string() +
         (ascending_ ? ", asc)" : ", desc)");
}

std::string IndexScanPlan::to_string() const {
  return "IndexScan(" + target_.to_string() + ", " + range_.to_string() +
         (ascending_ ? ", asc)" : ", desc)");
}

std::string RangeUnionPlan::to_string() const {
  std::string s =
      "RangeUnion(" + target_.to_string() + ", " + ranges_to_string(ranges_);
  if (!exclude_keys_.empty()) {
    s += ", exclude=" + exclude_keys_.to_string();
  }
  s += ascending_ ? ", asc)" : ", desc)";
  return s;
}

std::string FilterPlan::to_string() const {
  return "Filter(" + (condition_ ? condition_->to_string() : "-") + ")";
}

std::string SortPlan::to_string() const {
  std::string s = is_top_n_ ? "TopN(order_by=[" : "Sort(order_by=[";
  for (size_t i = 0; i < order_by_.size(); ++i) {
    if (i > 0) {
      s += ", ";
    }
    s += order_by_[i].column.display_name();
    s += order_by_[i].direction == sql::OrderDirection::ASC ? " ASC" : " DESC";
  }
  s += "]";
  if (is_top_n_) {
    s += " n=" + std::to_string(top_n_);
  }
  s += ")";
  return s;
}

std::string LimitPlan::to_string() const {
  std::string s = "Limit(limit=";
  s += limit_.has_limit() ? std::to_string(limit_.limit_value()) : "none";
  s += " offset=";
  s += limit_.has_offset() ? std::to_string(limit_.offset_value()) : "0";
  s += ")";
  return s;
}

std::string ProjectPlan::to_string() const {
  std::string s = "Project([";
  for (size_t i = 0; i < columns_.size(); ++i) {
    if (i > 0) {
      s += ", ";
    }
    s += columns_[i].display_name();
  }
  s += "])";
  return s;
}

std::string InsertPlan::to_string() const {
  return "Insert(" + writes_to_string(query_) + ")";
}

std::string UpdatePlan::to_string() const {
  return "Update(" + writes_to_string(query_) + ")";
}

std::string DeletePlan::to_string() const {
  return "Delete(" + writes_to_string(query_) + ")";
}

std::string CreateDatabasePlan::to_string() const {
  return "CreateDatabase(" + writes_to_string(query_) + ")";
}

std::string DropDatabasePlan::to_string() const {
  return "DropDatabase(" + writes_to_string(query_) + ")";
}

std::string CreateTablePlan::to_string() const {
  return "CreateTable(" + writes_to_string(query_) + ")";
}

std::string DropTablePlan::to_string() const {
  return "DropTable(" + writes_to_string(query_) + ")";
}

std::string UseDatabasePlan::to_string() const {
  return "UseDatabase(" + writes_to_string(query_) + ")";
}

std::string plan_tree_to_string(const PlanNode &root) {
  std::string text;
  int depth = 0;
  const PlanNode *node = &root;
  while (node != nullptr) {
    text.append(static_cast<size_t>(depth) * 2, ' ');
    text += node->to_string();
    text += "\n";
    node = node->child();
    ++depth;
  }
  return text;
}

} // namespace plan
