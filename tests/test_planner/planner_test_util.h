// tests/test_planner/planner_test_util.h
//
// planner 测试的公共脚手架：
//   - 造 Catalog / schema（users 有 INT 主键、logs 无主键、accounts
//   是字符串主键）
//   - 走真实链路（parser -> StatementBuilder）造 Query
//   - 用 sql_types 的三值逻辑求值"计划是否保留该行"，用来做语义对照
#pragma once

#include <optional>
#include <string>

#include "memory_catalog.h"
#include "parser/parser.h"
#include "planner/optimizer.h"
#include "planner/plan.h"
#include "planner/rewriter.h"
#include "sql_types/condition_types.h"
#include "sql_types/sql_truth.h"
#include "statement/stmt_builder.h"

namespace plantest {

// 无主键表：只能全表扫描
inline sql::TableSchema make_logs_schema() {
  sql::TableSchema schema(sql::Identifier("logs"));
  schema.add_column(sql::Identifier("message"), sql::DataType::VARCHAR, 64u,
                    false, true);
  return schema;
}

// 字符串主键表：验证 VARCHAR 主键也能走点查/区间
inline sql::TableSchema make_accounts_schema() {
  sql::TableSchema schema(sql::Identifier("accounts"));
  schema.add_column(sql::Identifier("name"), sql::DataType::VARCHAR, 32u, true,
                    false);
  schema.add_column(sql::Identifier("balance"), sql::DataType::INT, false,
                    true);
  return schema;
}

inline void setup_catalog(MemoryCatalog &catalog) {
  catalog.reset();
  catalog.set_open(true);
  catalog.create_database(sql::Identifier("testdb"));
  catalog.use_database(sql::Identifier("testdb"));
  catalog.create_table(sql::Identifier("testdb"), make_users_schema());
  catalog.create_table(sql::Identifier("testdb"), make_logs_schema());
  catalog.create_table(sql::Identifier("testdb"), make_accounts_schema());
}

// SQL -> Query（parser + StatementBuilder）
inline std::optional<sql::Query> build_query(const std::string &sql) {
  parser::Parser parser;
  // 语法要求语句以 ';' 结束，测试里省掉它更贴近阅读习惯
  const std::string terminated = sql.back() == ';' ? sql : sql + ";";
  auto parsed = parser.parse(terminated);
  if (!parsed.success || parsed.ast == nullptr) {
    return std::nullopt;
  }
  stmt::StatementBuilder builder;
  auto built = builder.build(parsed.ast.get());
  if (!built.has_value()) {
    return std::nullopt;
  }
  return std::optional<sql::Query>(std::move(*built));
}

// 候选空间是否包含该值：(⋃ ranges) 再减去 exclude_keys
inline bool candidate_contains(const plan::OptimizedQuery &oq,
                               const sql::Value &value) {
  if (oq.exclude_keys.contains(value)) {
    return false;
  }
  for (const auto &range : oq.ranges) {
    if (range.contains(value)) {
      return true;
    }
  }
  return false;
}

inline sql::ValueLookup lookup_of(const sql::Identifier &column,
                                  const sql::Value &value) {
  return [&column, &value](const sql::Identifier &name) -> const sql::Value * {
    return name == column ? &value : nullptr;
  };
}

// 计划的语义：候选空间 + remaining_filter（WHERE 只保留 TRUE）
inline bool plan_keeps(const plan::OptimizedQuery &oq,
                       const sql::Identifier &column, const sql::Value &value) {
  if (!candidate_contains(oq, value)) {
    return false;
  }
  if (!oq.remaining_filter) {
    return true;
  }
  return sql::where_keeps(
      sql::evaluate_condition(*oq.remaining_filter, lookup_of(column, value)));
}

// 原始谓词的语义（参照物）
inline bool sql_keeps(const sql::Condition *where,
                      const sql::Identifier &column, const sql::Value &value) {
  if (where == nullptr) {
    return true;
  }
  return sql::where_keeps(
      sql::evaluate_condition(*where, lookup_of(column, value)));
}

// ---- 计划树辅助 ----

// 沿 child() 找到第一个指定类型的节点（找不到返回 nullptr）
inline const plan::PlanNode *find_node(const plan::PlanNode *root,
                                       plan::PlanType type) {
  for (const plan::PlanNode *node = root; node != nullptr;
       node = node->child()) {
    if (node->type() == type) {
      return node;
    }
  }
  return nullptr;
}

// 沿 child() 找到第一个"携带 OptimizedQuery"的节点（扫描/写/DDL）
inline const plan::QueryPlanNode *find_query_node(const plan::PlanNode *root) {
  for (const plan::PlanNode *node = root; node != nullptr;
       node = node->child()) {
    if (const auto *query_node =
            dynamic_cast<const plan::QueryPlanNode *>(node)) {
      return query_node;
    }
  }
  return nullptr;
}

} // namespace plantest
