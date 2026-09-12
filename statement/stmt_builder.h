// stmt_builder.h
//
// StatementBuilder：AST -> sql::Query
//
// 职责边界：
//   - 只做"结构转换 + 结构校验"（节点类型、必要字段、值的形态）；
//   - 不查 Catalog、不做表/列存在性与类型校验 —— 那是 StatementValidator 的事；
//   - **不接管 AST 所有权**：AST 由解析方（parser::ParseResult / ASTNodePtr）
//     负责 free_ast，这里只读取。
//
// 错误处理：完全无状态 —— 错误码与错误信息随 std::expected 一起返回，
// 对象可以反复使用/并发使用，不需要 error()/has_error() 之类的状态查询。
#ifndef STATEMENT_STMT_BUILDER_H
#define STATEMENT_STMT_BUILDER_H

#include <expected>
#include <string>

#include "parser/ast.h"
#include "sql_types/condition_types.h"
#include "sql_types/identifier.h"
#include "sql_types/query.h"
#include "sql_types/value.h"
#include "stmt_defs.h"

namespace stmt {

class StatementBuilder {
public:
  StatementBuilder() = default;
  ~StatementBuilder() = default;

  // AST -> Query；失败时错误值里同时带错误码与可读信息
  std::expected<sql::Query, StmtError> build(const ASTNode *ast);

private:
  // ---- 各语句类型 ----
  std::expected<sql::Query, StmtError> build_stmt_use(const ASTNode *ast);
  std::expected<sql::Query, StmtError> build_stmt_select(const ASTNode *ast);
  std::expected<sql::Query, StmtError> build_stmt_insert(const ASTNode *ast);
  std::expected<sql::Query, StmtError> build_stmt_update(const ASTNode *ast);
  std::expected<sql::Query, StmtError> build_stmt_delete(const ASTNode *ast);
  std::expected<sql::Query, StmtError>
  build_stmt_create_database(const ASTNode *ast);
  std::expected<sql::Query, StmtError>
  build_stmt_drop_database(const ASTNode *ast);
  std::expected<sql::Query, StmtError>
  build_stmt_create_table(const ASTNode *ast);
  std::expected<sql::Query, StmtError>
  build_stmt_drop_table(const ASTNode *ast);

  // ---- 片段转换 ----
  std::expected<sql::Value, StmtError> build_value(const ASTNode *ast);
  std::expected<std::vector<sql::Value>, StmtError>
  build_value_list(const ASTNode *ast);
  std::expected<std::vector<sql::Identifier>, StmtError>
  build_column_list(const ASTNode *ast);
  std::expected<std::vector<sql::ColumnRef>, StmtError>
  build_select_columns(const ASTNode *ast);
  std::expected<std::vector<sql::OrderByItem>, StmtError>
  build_order_by(const ASTNode *ast);
  std::expected<sql::LimitClause, StmtError> build_limit(const ASTNode *ast);
  std::expected<sql::ConditionPtr, StmtError>
  build_condition(const ASTNode *ast);
  std::expected<std::vector<sql::ColumnDef>, StmtError>
  build_column_defs(const ASTNode *ast);

  // ---- 工具 ----
  // 构造错误值（不修改对象状态）
  static StmtError make_error(StmtErrorCode code, std::string message,
                              SSpan span = sspan_unknown());
  static bool is_list_node(const ASTNode *ast);
  static const ASTNodeList *as_list(const ASTNode *ast);
};

} // namespace stmt

#endif // STATEMENT_STMT_BUILDER_H
