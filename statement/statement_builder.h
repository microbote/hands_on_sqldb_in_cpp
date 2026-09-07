// statement_builder.h
#ifndef QUERY_STATEMENT_BUILDER_H
#define QUERY_STATEMENT_BUILDER_H

#include <memory>
#include <string>

#include "parser/ast.h"
#include "relation/database_manager.h"
#include "condition.h"
#include "statement.h"

namespace query {
// build the condition expr tree from ast's where condition tree
static std::unique_ptr<ConditionExpr> build_condition_expr_from_ast(
    const ASTNode* ast_where);

// ============================================================
// Statement 构建器（AST → Statement + 语义验证）
// ============================================================
class StatementBuilder {
 public:
  explicit StatementBuilder(std::shared_ptr<sql::DatabaseManager> db_manager,
                            const std::string& current_db = "");
  ~StatementBuilder() = default;

  // 从 AST 构建 Statement
  std::unique_ptr<Statement> build(ASTNode* ast);

  // 错误信息
  bool has_error() const { return !error_.empty(); }
  const std::string& get_error() const { return error_; }
  void clear_error() { error_.clear(); }

  // 当前数据库
  void set_current_db(const std::string& db) { current_db_ = db; }
  const std::string& current_db() const { return current_db_; }

 private:
  // ----- 具体构建方法 -----
  std::unique_ptr<UseStatement> build_use(ASTNode* node);
  std::unique_ptr<SelectStatement> build_select(ASTNode* node);
  std::unique_ptr<InsertStatement> build_insert(ASTNode* node);
  std::unique_ptr<UpdateStatement> build_update(ASTNode* node);
  std::unique_ptr<DeleteStatement> build_delete(ASTNode* node);

  // ----- 辅助构建方法 -----
  std::unique_ptr<ConditionExpr> build_condition(
      ASTNode* node, const sql::TableSchema& schema);
  std::vector<std::string> build_column_list(ASTNode* node);
  std::vector<sql::Value> build_value_list(ASTNode* node);
  std::vector<std::pair<std::string, sql::Value>> build_assignments(
      ASTNode* node, const sql::TableSchema& schema);
  sql::Value ast_to_value(ASTNode* node);
  CompareOp get_compare_op(const char* op);

  // ----- 验证方法 -----
  bool validate_table(const std::string& db_name,
                      const std::string& table_name);
  bool validate_column(const std::string& db_name,
                       const std::string& table_name,
                       const std::string& column_name);
  sql::TableSchema get_table_schema(const std::string& db_name,
                                    const std::string& table_name);

  // ----- 成员变量 -----
  std::shared_ptr<sql::DatabaseManager> db_manager_;
  std::string current_db_;
  std::string error_;
};

}  // namespace query

#endif  // QUERY_STATEMENT_BUILDER_H