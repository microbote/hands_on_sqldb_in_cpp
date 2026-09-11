// stmt_validator.h
//
// StatementValidator：对 sql::Query 做语义校验（查 Catalog 元数据）。
//
// 职责边界：
//   - 表/库是否存在、列是否存在、值类型与取值范围是否匹配；
//   - 不做 AST 转换（那是 StatementBuilder 的事），不改数据。
//
// 错误处理：完全无状态 —— 返回 std::expected<void, StmtError>，
// 错误码与信息随调用返回；同一个 validator 可以反复使用/并发使用。
#pragma once

#include <expected>
#include <string>

#include "catalog.h"
#include "schema.h"
#include "sql_types/condition.h"
#include "sql_types/identifier.h"
#include "sql_types/query.h"
#include "sql_types/value.h"
#include "stmt_defs.h"

namespace stmt {

class StatementValidator {
public:
  explicit StatementValidator(const sql::Catalog &catalog)
      : catalog_(catalog) {}

  // 校验一条 Query；成功返回空 expected，失败返回带上下文的错误值
  std::expected<void, StmtError> validate(const sql::Query &stmt) const;

private:
  // ---- 分派 ----
  std::expected<void, StmtError> validate_ddl(const sql::Query &stmt) const;
  std::expected<void, StmtError> validate_dml(const sql::Query &stmt) const;
  std::expected<void, StmtError> validate_ctrl(const sql::Query &stmt) const;

  // ---- DDL ----
  std::expected<void, StmtError>
  validate_create_database(const sql::Query &stmt) const;
  std::expected<void, StmtError>
  validate_drop_database(const sql::Query &stmt) const;
  std::expected<void, StmtError>
  validate_create_table(const sql::Query &stmt) const;
  std::expected<void, StmtError>
  validate_drop_table(const sql::Query &stmt) const;

  // ---- DML ----
  std::expected<void, StmtError> validate_select(const sql::Query &stmt) const;
  std::expected<void, StmtError> validate_insert(const sql::Query &stmt) const;
  std::expected<void, StmtError> validate_update(const sql::Query &stmt) const;
  std::expected<void, StmtError> validate_delete(const sql::Query &stmt) const;

  // ---- CTRL ----
  std::expected<void, StmtError> validate_use(const sql::Query &stmt) const;

  // ---- 辅助 ----
  // 解析表所在的库：显式指定优先，否则用当前库
  sql::Identifier resolve_database(const sql::Identifier &explicit_db) const;
  std::expected<void, StmtError>
  require_database(const sql::Identifier &db) const;
  std::expected<sql::TableSchema, StmtError>
  load_table(const sql::Identifier &db, const sql::Identifier &table) const;
  std::expected<void, StmtError>
  require_column(const sql::TableSchema &schema,
                 const sql::Identifier &column) const;
  // 检查条件树里引用到的列都存在
  std::expected<void, StmtError>
  check_condition_columns(const sql::Condition *cond,
                          const sql::TableSchema &schema) const;
  // 值是否可以写入该列（类型族 + 范围 + 长度 + NULL 约束）
  std::expected<void, StmtError> check_value(const sql::ColumnDef &column,
                                             const sql::Value &value) const;
  static StmtError make_error(StmtErrorCode code, std::string message);

  const sql::Catalog &catalog_;
};

} // namespace stmt
