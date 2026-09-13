// stmt_validator.cpp
#include "stmt_validator.h"

#include <set>
#include <string>
#include <utility>
#include <vector>

#include "sql_types/condition_types.h"
#include "sql_types/condition_visitor.h"

namespace stmt {

namespace {

// sql_types 的 SchemaError -> stmt 的错误码
StmtErrorCode map_schema_error(sql::SchemaError err) {
  switch (err) {
  case sql::SchemaError::OK:
    return StmtErrorCode::OK;
  case sql::SchemaError::DUPLICATE_PRIMARY_KEY:
    return StmtErrorCode::DUPLICATE_PRIMARY_KEY;
  case sql::SchemaError::NO_PRIMARY_KEY:
    return StmtErrorCode::NO_PRIMARY_KEY;
  case sql::SchemaError::DUPLICATE_COLUMN_NAME:
    return StmtErrorCode::DUPLICATE_COLUMN;
  case sql::SchemaError::COLUMN_NOT_FOUND:
    return StmtErrorCode::COLUMN_NOT_FOUND;
  case sql::SchemaError::COLUMN_SIZE_MISMATCH:
    return StmtErrorCode::COLUMN_COUNT_MISMATCH;
  case sql::SchemaError::COLUMN_TYPE_MISMATCH:
    return StmtErrorCode::COLUMN_TYPE_MISMATCH;
  case sql::SchemaError::COLUMN_ATTR_NULL_MISMATCH:
    return StmtErrorCode::COLUMN_ATTR_NULL_MISMATCH;
  case sql::SchemaError::VALUE_OUT_OF_RANGE:
    return StmtErrorCode::VALUE_OUT_OF_RANGE;
  default:
    return StmtErrorCode::INVALID_SCHEMA;
  }
}

// 收集条件树里出现的所有列名
class ColumnCollector : public sql::ConditionVisitorBase {
public:
  std::vector<sql::Identifier> columns;

  void visit(const sql::CompareCondition &cond) override {
    columns.push_back(cond.column());
  }
  void visit(const sql::InCondition &cond) override {
    columns.push_back(cond.column());
  }
};

void collect_condition_columns(const sql::Condition *cond,
                               std::vector<sql::Identifier> &out) {
  if (cond == nullptr) {
    return;
  }
  ColumnCollector collector;
  sql::walk_condition_pre_order(*cond, collector);
  for (const auto &column : collector.columns) {
    out.push_back(column);
  }
}

// 便捷返回
inline std::expected<void, StmtError> ok() { return {}; }

inline std::expected<void, StmtError> failed(StmtError error) {
  return std::unexpected(std::move(error));
}

} // namespace

StmtError StatementValidator::make_error(StmtErrorCode code,
                                         std::string message,
                                         const sql::Identifier &target) const {
  StmtError error(code, std::move(message));
  if (resolver_ && !target.empty()) {
    error.span = resolver_(target);
  }
  return error;
}

// ============================================================
// 入口
// ============================================================
std::expected<void, StmtError>
StatementValidator::validate(const sql::Query &stmt) const {
  // Catalog 未打开：每次调用都重新判断，不依赖构造时的状态
  if (!catalog_.is_open()) {
    return failed(make_error(StmtErrorCode::VALIDATOR_NOT_INIT,
                             "catalog is not open; open a database first"));
  }
  if (stmt.type() == sql::QueryType::UNKNOWN) {
    return failed(
        make_error(StmtErrorCode::UNKNOWN_STMT_TYPE, "unknown statement type"));
  }
  // 事务控制语句没有库/表/列可校验：执行期的会话状态检查（是否已在事务里、
  // 能否开始事务）需要连接状态，属于 session 层的事
  if (stmt.is_transaction()) {
    return ok();
  }
  if (stmt.is_ddl()) {
    return validate_ddl(stmt);
  }
  if (stmt.is_dml()) {
    return validate_dml(stmt);
  }
  if (stmt.is_ctrl()) {
    return validate_ctrl(stmt);
  }
  return failed(make_error(StmtErrorCode::UNKNOWN_STMT_TYPE,
                           "statement is neither DDL/DML/CTRL"));
}

std::expected<void, StmtError>
StatementValidator::validate_ddl(const sql::Query &stmt) const {
  switch (stmt.type()) {
  case sql::QueryType::CREATE_DATABASE:
    return validate_create_database(stmt);
  case sql::QueryType::DROP_DATABASE:
    return validate_drop_database(stmt);
  case sql::QueryType::CREATE_TABLE:
    return validate_create_table(stmt);
  case sql::QueryType::DROP_TABLE:
    return validate_drop_table(stmt);
  default:
    return failed(
        make_error(StmtErrorCode::UNKNOWN_STMT_TYPE, "unknown DDL statement"));
  }
}

std::expected<void, StmtError>
StatementValidator::validate_dml(const sql::Query &stmt) const {
  switch (stmt.type()) {
  case sql::QueryType::SELECT:
    return validate_select(stmt);
  case sql::QueryType::INSERT:
    return validate_insert(stmt);
  case sql::QueryType::UPDATE:
    return validate_update(stmt);
  case sql::QueryType::DELETE:
    return validate_delete(stmt);
  default:
    return failed(
        make_error(StmtErrorCode::UNKNOWN_STMT_TYPE, "unknown DML statement"));
  }
}

std::expected<void, StmtError>
StatementValidator::validate_ctrl(const sql::Query &stmt) const {
  switch (stmt.type()) {
  case sql::QueryType::USE_DATABASE:
    return validate_use(stmt);
  default:
    return failed(make_error(StmtErrorCode::UNKNOWN_STMT_TYPE,
                             "unknown control statement"));
  }
}

// ============================================================
// 辅助
// ============================================================
sql::Identifier
StatementValidator::resolve_database(const sql::Identifier &explicit_db) const {
  if (!explicit_db.empty()) {
    return explicit_db;
  }
  return catalog_.current_database(); // 未选中时返回空
}

std::expected<void, StmtError>
StatementValidator::require_database(const sql::Identifier &db) const {
  if (db.empty()) {
    return failed(make_error(StmtErrorCode::DATABASE_NOT_FOUND,
                             "no database selected; use USE <database> first"));
  }
  if (!catalog_.database_exists(db)) {
    return failed(make_error(StmtErrorCode::DATABASE_NOT_FOUND,
                             "database not found: " + db.str(), db));
  }
  return ok();
}

std::expected<sql::TableSchema, StmtError>
StatementValidator::load_table(const sql::Identifier &db,
                               const sql::Identifier &table) const {
  if (table.empty()) {
    return std::unexpected(
        make_error(StmtErrorCode::TABLE_NOT_FOUND, "table name is empty"));
  }
  if (auto db_check = require_database(db); !db_check.has_value()) {
    return std::unexpected(db_check.error());
  }
  auto schema = catalog_.get_table_schema(db, table);
  if (!schema.has_value()) {
    return std::unexpected(
        make_error(StmtErrorCode::TABLE_NOT_FOUND,
                   "table not found: " + db.str() + "." + table.str(), table));
  }
  return std::move(*schema);
}

std::expected<void, StmtError>
StatementValidator::require_column(const sql::TableSchema &schema,
                                   const sql::Identifier &column) const {
  if (schema.column(column) == nullptr) {
    return failed(make_error(StmtErrorCode::COLUMN_NOT_FOUND,
                             "column not found: " + column.str(), column));
  }
  return ok();
}

std::expected<void, StmtError> StatementValidator::check_condition_columns(
    const sql::Condition *cond, const sql::TableSchema &schema) const {
  std::vector<sql::Identifier> columns;
  collect_condition_columns(cond, columns);
  for (const auto &column : columns) {
    if (auto err = require_column(schema, column); !err.has_value()) {
      return err;
    }
  }
  return ok();
}

std::expected<void, StmtError>
StatementValidator::check_value(const sql::ColumnDef &column,
                                const sql::Value &value,
                                SSpan value_span) const {
  // 直接复用 sql_types 的列值校验（类型族 + 范围 + 长度 + NOT NULL），
  // 这里不需要 schema 的其它信息，所以用一个空 schema 调用即可。
  static const sql::TableSchema empty_schema;
  const auto err = empty_schema.validate_value(column, value);
  if (err != sql::SchemaError::OK) {
    StmtError error(map_schema_error(err),
                    std::string(sql::TableSchema::error_message(err)) + " @ " +
                        column.name.str());
    // 定位优先级：值自身的位置 > 列名在 SQL 里的位置 > 未知
    if (sspan_valid(value_span)) {
      error.span = value_span;
    } else if (resolver_ && !column.name.empty()) {
      error.span = resolver_(column.name);
    }
    return failed(std::move(error));
  }
  return ok();
}

// ============================================================
// DDL
// ============================================================
std::expected<void, StmtError>
StatementValidator::validate_create_database(const sql::Query &stmt) const {
  const auto *query = stmt.create_database();
  if (query == nullptr) {
    return failed(make_error(StmtErrorCode::INVALID_SCHEMA,
                             "invalid CREATE DATABASE query"));
  }
  if (catalog_.database_exists(query->database)) {
    return failed(make_error(
        StmtErrorCode::DATABASE_ALREADY_EXISTS,
        "database already exists: " + query->database.str(), query->database));
  }
  return ok();
}

std::expected<void, StmtError>
StatementValidator::validate_drop_database(const sql::Query &stmt) const {
  const auto *query = stmt.drop_database();
  if (query == nullptr) {
    return failed(make_error(StmtErrorCode::INVALID_SCHEMA,
                             "invalid DROP DATABASE query"));
  }
  return require_database(query->database);
}

std::expected<void, StmtError>
StatementValidator::validate_create_table(const sql::Query &stmt) const {
  const auto *query = stmt.create_table();
  if (query == nullptr) {
    return failed(make_error(StmtErrorCode::INVALID_SCHEMA,
                             "invalid CREATE TABLE query"));
  }
  const auto db = resolve_database(query->database);
  if (auto db_check = require_database(db); !db_check.has_value()) {
    return db_check;
  }
  if (catalog_.table_exists(db, query->table)) {
    return failed(make_error(StmtErrorCode::TABLE_ALREADY_EXISTS,
                             "table already exists: " + query->table.str(),
                             query->table));
  }

  // 复用 sql_types 的 schema 校验（列名重复/主键唯一/长度声明...）
  sql::TableSchema schema(query->table);
  for (const auto &column : query->columns) {
    schema.add_column(column);
  }
  const auto schema_err = schema.validate();
  if (schema_err != sql::SchemaError::OK) {
    return failed(make_error(map_schema_error(schema_err),
                             std::string("invalid table schema: ") +
                                 sql::TableSchema::error_message(schema_err)));
  }
  return ok();
}

std::expected<void, StmtError>
StatementValidator::validate_drop_table(const sql::Query &stmt) const {
  const auto *query = stmt.drop_table();
  if (query == nullptr) {
    return failed(
        make_error(StmtErrorCode::INVALID_SCHEMA, "invalid DROP TABLE query"));
  }
  auto table = load_table(resolve_database(query->database), query->table);
  if (!table.has_value()) {
    return std::unexpected(table.error());
  }
  return ok();
}

// ============================================================
// DML
// ============================================================
std::expected<void, StmtError>
StatementValidator::validate_select(const sql::Query &stmt) const {
  const auto *query = stmt.select();
  if (query == nullptr) {
    return failed(
        make_error(StmtErrorCode::INVALID_SCHEMA, "invalid SELECT query"));
  }

  auto table = load_table(catalog_.current_database(), query->table);
  if (!table.has_value()) {
    return std::unexpected(table.error());
  }
  const sql::TableSchema &schema = *table;

  if (!query->select_all()) {
    for (const auto &ref : query->columns) {
      if (ref.is_wildcard()) {
        continue;
      }
      if (auto err = require_column(schema, ref.column); !err.has_value()) {
        return err;
      }
    }
  }
  if (auto err = check_condition_columns(query->where.get(), schema);
      !err.has_value()) {
    return err;
  }
  for (const auto &item : query->order_by) {
    if (auto err = require_column(schema, item.column); !err.has_value()) {
      return err;
    }
  }
  return ok();
}

std::expected<void, StmtError>
StatementValidator::validate_insert(const sql::Query &stmt) const {
  const auto *query = stmt.insert();
  if (query == nullptr) {
    return failed(
        make_error(StmtErrorCode::INVALID_SCHEMA, "invalid INSERT query"));
  }

  auto table = load_table(catalog_.current_database(), query->table);
  if (!table.has_value()) {
    return std::unexpected(table.error());
  }
  const sql::TableSchema &schema = *table;

  if (query->values.empty()) {
    return failed(make_error(StmtErrorCode::COLUMN_COUNT_MISMATCH,
                             "INSERT has no values"));
  }

  // 列清单：空 = 按 schema 顺序
  std::vector<const sql::ColumnDef *> target_columns;
  if (query->columns.empty()) {
    for (const auto &column : schema.columns()) {
      target_columns.push_back(&column);
    }
  } else {
    for (const auto &name : query->columns) {
      const auto *column = schema.column(name);
      if (column == nullptr) {
        return failed(make_error(StmtErrorCode::COLUMN_NOT_FOUND,
                                 "column not found: " + name.str(), name));
      }
      target_columns.push_back(column);
    }
  }

  for (size_t row_index = 0; row_index < query->values.size(); ++row_index) {
    const auto &row = query->values[row_index];
    if (row.size() != target_columns.size()) {
      return failed(make_error(StmtErrorCode::COLUMN_COUNT_MISMATCH,
                               "value count does not match column count"));
    }
    for (size_t i = 0; i < row.size(); ++i) {
      const SSpan *span = query->value_span(row_index, i);
      if (auto err = check_value(*target_columns[i], row[i],
                                 span != nullptr ? *span : sspan_unknown());
          !err.has_value()) {
        return err;
      }
    }
    // 显式列清单时，未提到的列必须可空（暂无 DEFAULT 支持）
    for (const auto &column : schema.columns()) {
      bool mentioned = false;
      for (const auto *target : target_columns) {
        if (target == &column) {
          mentioned = true;
          break;
        }
      }
      if (!mentioned && !column.nullable) {
        return failed(make_error(StmtErrorCode::COLUMN_ATTR_NULL_MISMATCH,
                                 "missing value for NOT NULL column: " +
                                     column.name.str(),
                                 column.name));
      }
    }
  }

  // ---- 主键冲突：**执行前**就查出来（越早越好）----
  //
  // 两类冲突：
  //   1) 这一批 VALUES 内部重复（纯静态检查，任何 Catalog 都能查）；
  //   2) 与表里已有的行冲突（需要 Catalog 支持数据探测；
  //      不支持就退回执行期 —— Table::insert 本来就会报重复主键）。
  // 在事务里这一点尤其值钱：语句在校验期就被拒，事务不会执行到一半才失败。
  if (auto err = check_primary_key_conflicts(schema, *query, target_columns);
      !err.has_value()) {
    return err;
  }
  return ok();
}

std::expected<void, StmtError> StatementValidator::check_primary_key_conflicts(
    const sql::TableSchema &schema, const sql::InsertQuery &query,
    const std::vector<const sql::ColumnDef *> &target_columns) const {
  if (!schema.has_primary_key()) {
    return ok();
  }
  const sql::ColumnDef *pk_column = schema.primary_key_column();
  if (pk_column == nullptr) {
    return ok();
  }

  // 主键列在这条 INSERT 的列清单里的位置。
  // 没给主键列 -> 不在这里报错（"NOT NULL 列缺值"那条检查会报）。
  size_t value_index = target_columns.size();
  for (size_t i = 0; i < target_columns.size(); ++i) {
    if (target_columns[i] == pk_column) {
      value_index = i;
      break;
    }
  }
  if (value_index >= target_columns.size()) {
    return ok();
  }

  const sql::Identifier &db = catalog_.current_database();
  // "同一个主键"用**存储编码**判断（和 Table::insert 落 key 的方式一致）
  std::set<sql::Key> seen;
  for (size_t row_index = 0; row_index < query.values.size(); ++row_index) {
    const sql::Value &pk = query.values[row_index][value_index];
    if (pk.is_null()) {
      continue; // NULL 主键：交给 NOT NULL / 执行期那条路径
    }
    const SSpan *span = query.value_span(row_index, value_index);
    const SSpan where = span != nullptr ? *span : sspan_unknown();
    const sql::Key key = pk.to_key(pk_column->type);

    // 1) 这一批 VALUES 内部重复
    if (!seen.insert(key).second) {
      return failed(StmtError(
          StmtErrorCode::DUPLICATE_PRIMARY_KEY,
          "duplicate primary key in this INSERT: " + pk.to_string(), where));
    }

    // 2) 与表里已有的行冲突（Catalog 不支持探测时跳过，执行期会报）
    auto exists = catalog_.primary_key_exists(db, query.table, pk);
    if (exists.has_value() && *exists) {
      return failed(StmtError(StmtErrorCode::DUPLICATE_PRIMARY_KEY,
                              "duplicate primary key: " + pk.to_string() +
                                  " already exists in " + query.table.str(),
                              where));
    }
  }
  return ok();
}

std::expected<void, StmtError>
StatementValidator::validate_update(const sql::Query &stmt) const {
  const auto *query = stmt.update();
  if (query == nullptr) {
    return failed(
        make_error(StmtErrorCode::INVALID_SCHEMA, "invalid UPDATE query"));
  }

  auto table = load_table(catalog_.current_database(), query->table);
  if (!table.has_value()) {
    return std::unexpected(table.error());
  }
  const sql::TableSchema &schema = *table;

  if (query->assignments.empty()) {
    return failed(make_error(StmtErrorCode::EMPTY_STATEMENT,
                             "UPDATE has no assignments"));
  }

  for (const auto &assignment : query->assignments) {
    const auto *column = schema.column(assignment.column);
    if (column == nullptr) {
      return failed(make_error(StmtErrorCode::COLUMN_NOT_FOUND,
                               "column not found: " + assignment.column.str(),
                               assignment.column));
    }
    if (auto err =
            check_value(*column, assignment.value, assignment.value_span);
        !err.has_value()) {
      return err;
    }
    // 主键不允许被更新（避免行迁移；如需支持再放开）
    if (column->primary_key) {
      return failed(make_error(StmtErrorCode::INVALID_SCHEMA,
                               "primary key column cannot be updated: " +
                                   column->name.str(),
                               column->name));
    }
  }

  return check_condition_columns(query->where.get(), schema);
}

std::expected<void, StmtError>
StatementValidator::validate_delete(const sql::Query &stmt) const {
  const auto *query = stmt.delete_();
  if (query == nullptr) {
    return failed(
        make_error(StmtErrorCode::INVALID_SCHEMA, "invalid DELETE query"));
  }

  auto table = load_table(catalog_.current_database(), query->table);
  if (!table.has_value()) {
    return std::unexpected(table.error());
  }
  return check_condition_columns(query->where.get(), *table);
}

// ============================================================
// CTRL
// ============================================================
std::expected<void, StmtError>
StatementValidator::validate_use(const sql::Query &stmt) const {
  const auto *query = stmt.use_database();
  if (query == nullptr) {
    return failed(
        make_error(StmtErrorCode::INVALID_SCHEMA, "invalid USE query"));
  }
  if (!catalog_.database_exists(query->database)) {
    return failed(make_error(StmtErrorCode::DATABASE_NOT_FOUND,
                             "database not found: " + query->database.str(),
                             query->database));
  }
  return ok();
}

} // namespace stmt
