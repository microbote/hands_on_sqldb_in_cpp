// statement_builder.h
#ifndef STATEMENT_BUILDER_H
#define STATEMENT_BUILDER_H

#include <memory>
#include <string>

#include "ast.h"
#include "statement.h"

namespace sql {

class StatementBuilder {
 public:
  explicit StatementBuilder(std::shared_ptr<storage::StorageEngine> engine);
  ~StatementBuilder() = default;

  // 从 AST 构建并验证 Statement
  std::unique_ptr<Statement> build(ASTNode* ast);

  // 获取错误信息
  bool has_error() const { return !error_.empty(); }
  const std::string& get_error() const { return error_; }
  void clear_error() { error_.clear(); }

 private:
  // ===== 具体构建方法（同时做验证） =====
  std::unique_ptr<UseStatement> build_use(ASTNode* node);
  std::unique_ptr<SelectStatement> build_select(ASTNode* node);
  std::unique_ptr<InsertStatement> build_insert(ASTNode* node);
  std::unique_ptr<UpdateStatement> build_update(ASTNode* node);
  std::unique_ptr<DeleteStatement> build_delete(ASTNode* node);

  // ===== 验证辅助方法 =====
  bool validate_table(const std::string& name);
  bool validate_column(const std::string& table, const std::string& column);
  bool validate_value_type(const Value& value, DataType expected);
  bool validate_conditions(const std::vector<Condition>& conditions,
                           const std::string& table);

  // ===== 转换辅助方法 =====
  std::vector<Condition> build_conditions(ASTNode* node,
                                          const std::string& table);
  Value ast_to_value(ASTNode* node);
  CompareOp get_compare_op(const char* op);
  std::vector<std::string> build_column_list(ASTNode* node,
                                             const std::string& table);
  std::vector<Value> build_value_list(ASTNode* node);
  std::vector<std::pair<std::string, Value>> build_assignments(
      ASTNode* node, const std::string& table);

 private:
  std::shared_ptr<storage::StorageEngine> engine_;
  std::string error_;
};

}  // namespace sql

#endif  // STATEMENT_BUILDER_H