// stmt_builder.cpp
#include "stmt_builder.h"

#include <string>
#include <utility>
#include <vector>

#include "sql_types/compare_op.h"
#include "sql_types/query_clause.h"

namespace stmt {

StmtError StatementBuilder::make_error(StmtErrorCode code,
                                       std::string message) {
  return StmtError(code, std::move(message));
}

bool StatementBuilder::is_list_node(const ASTNode *ast) {
  return ast != nullptr && ast->type == NODE_LIST;
}

const ASTNodeList *StatementBuilder::as_list(const ASTNode *ast) {
  if (!is_list_node(ast)) {
    return nullptr;
  }
  return reinterpret_cast<const ASTNodeList *>(ast->data);
}

// ============================================================
// 入口
// ============================================================
std::expected<sql::Query, StmtError> StatementBuilder::build(ASTNode *ast) {
  if (ast == nullptr) {
    return std::unexpected(
        make_error(StmtErrorCode::AST_IS_NULL, "AST is null"));
  }

  switch (ast->type) {
  case NODE_USE:
    return build_stmt_use(ast);
  case NODE_SELECT:
    return build_stmt_select(ast);
  case NODE_INSERT:
    return build_stmt_insert(ast);
  case NODE_UPDATE:
    return build_stmt_update(ast);
  case NODE_DELETE:
    return build_stmt_delete(ast);
  case NODE_CREATE_DATABASE:
    return build_stmt_create_database(ast);
  case NODE_DROP_DATABASE:
    return build_stmt_drop_database(ast);
  case NODE_CREATE_TABLE:
    return build_stmt_create_table(ast);
  case NODE_DROP_TABLE:
    return build_stmt_drop_table(ast);
  default:
    return std::unexpected(make_error(StmtErrorCode::UNKNOWN_AST_Type,
                                      std::string("unknown AST node type: ") +
                                          node_type_to_string(ast->type)));
  }
}

// ============================================================
// 值与列表
// ============================================================
std::expected<sql::Value, StmtError>
StatementBuilder::build_value(ASTNode *ast) {
  if (ast == nullptr) {
    return std::unexpected(
        make_error(StmtErrorCode::INVALID_AST_NODE, "value node is null"));
  }

  switch (ast->type) {
  case NODE_NUMBER: {
    const auto *data = reinterpret_cast<const NumberNode *>(ast->data);
    return sql::Value(static_cast<int64_t>(data->value));
  }
  case NODE_STRING: {
    const auto *data = reinterpret_cast<const StringNode *>(ast->data);
    return sql::Value(std::string(data->value != nullptr ? data->value : ""));
  }
  case NODE_LITERAL: {
    const auto *data = reinterpret_cast<const LiteralNode *>(ast->data);
    switch (data->kind) {
    case LITERAL_NULL:
      return sql::Value(); // NULL 与空串是两回事
    case LITERAL_TRUE:
      return sql::Value(true);
    case LITERAL_FALSE:
      return sql::Value(false);
    default:
      break;
    }
    return std::unexpected(
        make_error(StmtErrorCode::INVALID_AST_NODE, "unknown literal kind"));
  }
  default:
    return std::unexpected(make_error(StmtErrorCode::UNSUPPORTED_AST_NODE,
                                      std::string("unsupported value node: ") +
                                          node_type_to_string(ast->type)));
  }
}

std::expected<std::vector<sql::Value>, StmtError>
StatementBuilder::build_value_list(ASTNode *ast) {
  std::vector<sql::Value> values;
  const ASTNodeList *list = as_list(ast);
  if (list == nullptr) {
    return std::unexpected(
        make_error(StmtErrorCode::INVALID_AST_NODE, "value list is missing"));
  }
  for (const ASTNode *item = list->head; item != nullptr; item = item->next) {
    auto value = build_value(const_cast<ASTNode *>(item));
    if (!value.has_value()) {
      return std::unexpected(value.error());
    }
    values.push_back(std::move(*value));
  }
  return values;
}

std::expected<std::vector<sql::Identifier>, StmtError>
StatementBuilder::build_column_list(ASTNode *ast) {
  std::vector<sql::Identifier> columns;
  if (ast == nullptr) {
    return columns; // 允许缺省（INSERT 不写列名）
  }
  const ASTNodeList *list = as_list(ast);
  if (list == nullptr) {
    return std::unexpected(make_error(StmtErrorCode::INVALID_AST_NODE,
                                      "column list is malformed"));
  }
  for (const ASTNode *item = list->head; item != nullptr; item = item->next) {
    if (item->type != NODE_IDENT) {
      return std::unexpected(make_error(StmtErrorCode::INVALID_AST_NODE,
                                        "column list contains non-identifier"));
    }
    const auto *ident = reinterpret_cast<const IdentNode *>(item->data);
    columns.emplace_back(ident->name != nullptr ? ident->name : "");
  }
  return columns;
}

std::expected<std::vector<sql::ColumnRef>, StmtError>
StatementBuilder::build_select_columns(ASTNode *ast) {
  std::vector<sql::ColumnRef> columns;
  if (ast == nullptr) {
    return columns; // SELECT *（空 = 通配）
  }
  const ASTNodeList *list = as_list(ast);
  if (list == nullptr) {
    return std::unexpected(make_error(StmtErrorCode::INVALID_AST_NODE,
                                      "select list is malformed"));
  }
  for (const ASTNode *item = list->head; item != nullptr; item = item->next) {
    if (item->type != NODE_IDENT) {
      return std::unexpected(make_error(StmtErrorCode::INVALID_AST_NODE,
                                        "select list contains non-identifier"));
    }
    const auto *ident = reinterpret_cast<const IdentNode *>(item->data);
    sql::ColumnRef ref;
    ref.column = sql::Identifier(ident->name != nullptr ? ident->name : "");
    columns.push_back(std::move(ref));
  }
  return columns;
}

std::expected<std::vector<sql::OrderByItem>, StmtError>
StatementBuilder::build_order_by(ASTNode *ast) {
  std::vector<sql::OrderByItem> items;
  if (ast == nullptr) {
    return items;
  }
  const ASTNodeList *list = as_list(ast);
  if (list == nullptr) {
    return std::unexpected(make_error(StmtErrorCode::INVALID_AST_NODE,
                                      "order by list is malformed"));
  }
  for (const ASTNode *item = list->head; item != nullptr; item = item->next) {
    if (item->type != NODE_ORDER) {
      return std::unexpected(
          make_error(StmtErrorCode::INVALID_AST_NODE,
                     "order by list contains non-order item"));
    }
    const auto *order = reinterpret_cast<const OrderNode *>(item->data);
    sql::OrderByItem out;
    out.column = sql::Identifier(order->column != nullptr ? order->column : "");
    out.direction = (order->direction == OP_DESC) ? sql::OrderDirection::DESC
                                                  : sql::OrderDirection::ASC;
    items.push_back(std::move(out));
  }
  return items;
}

std::expected<sql::LimitClause, StmtError>
StatementBuilder::build_limit(ASTNode *ast) {
  sql::LimitClause clause;
  if (ast == nullptr) {
    return clause;
  }
  if (ast->type != NODE_LIMIT) {
    return std::unexpected(
        make_error(StmtErrorCode::INVALID_AST_NODE, "limit node is malformed"));
  }
  const auto *limit = reinterpret_cast<const LimitNode *>(ast->data);
  if (limit->limit > 0) {
    clause.row_count = static_cast<size_t>(limit->limit);
  }
  if (limit->offset > 0) {
    clause.offset = static_cast<size_t>(limit->offset);
  }
  return clause;
}

std::expected<sql::ConditionPtr, StmtError>
StatementBuilder::build_condition(ASTNode *ast) {
  if (ast == nullptr) {
    return sql::ConditionPtr(nullptr); // 无条件
  }

  switch (ast->type) {
  case NODE_COMPARE: {
    const auto *data = reinterpret_cast<const CompareNode *>(ast->data);
    const auto op = sql::from_c(data->op);
    if (op == sql::CompareOp::UNKNOWN) {
      return std::unexpected(make_error(StmtErrorCode::INVALID_AST_NODE,
                                        "unknown comparison operator"));
    }
    sql::Value value;
    if (data->right != nullptr) {
      // IS [NOT] NULL 没有右值
      auto built = build_value(const_cast<ASTNode *>(data->right));
      if (!built.has_value()) {
        return std::unexpected(built.error());
      }
      value = std::move(*built);
    } else if (!sql::is_null_op(op)) {
      return std::unexpected(make_error(StmtErrorCode::INVALID_AST_NODE,
                                        "comparison is missing right operand"));
    }
    return sql::make_compare(
        sql::Identifier(data->column != nullptr ? data->column : ""), op,
        std::move(value));
  }
  case NODE_IN:
  case NODE_NOT_IN: {
    const auto *data = reinterpret_cast<const InNode *>(ast->data);
    auto values = build_value_list(const_cast<ASTNode *>(data->values));
    if (!values.has_value()) {
      return std::unexpected(values.error());
    }
    const bool is_not_in = (ast->type == NODE_NOT_IN) || (data->excluded != 0);
    return sql::make_in(
        sql::Identifier(data->column != nullptr ? data->column : ""), is_not_in,
        std::move(*values));
  }
  case NODE_BINARY_OP: {
    const auto *data = reinterpret_cast<const BinaryOpNode *>(ast->data);
    auto left = build_condition(const_cast<ASTNode *>(data->left));
    if (!left.has_value()) {
      return std::unexpected(left.error());
    }
    auto right = build_condition(const_cast<ASTNode *>(data->right));
    if (!right.has_value()) {
      return std::unexpected(right.error());
    }
    switch (data->op) {
    case OP_AND:
      return sql::make_and(std::move(*left), std::move(*right));
    case OP_OR:
      return sql::make_or(std::move(*left), std::move(*right));
    default:
      return std::unexpected(make_error(StmtErrorCode::INVALID_AST_NODE,
                                        "unknown logical operator"));
    }
  }
  case NODE_NOT: {
    const auto *data = reinterpret_cast<const NotNode *>(ast->data);
    auto child = build_condition(const_cast<ASTNode *>(data->child));
    if (!child.has_value()) {
      return std::unexpected(child.error());
    }
    return sql::make_not(std::move(*child));
  }
  default:
    return std::unexpected(
        make_error(StmtErrorCode::INVALID_AST_NODE,
                   std::string("malformed condition node: ") +
                       node_type_to_string(ast->type)));
  }
}

std::expected<std::vector<sql::ColumnDef>, StmtError>
StatementBuilder::build_column_defs(ASTNode *ast) {
  std::vector<sql::ColumnDef> columns;
  const ASTNodeList *list = as_list(ast);
  if (list == nullptr) {
    return std::unexpected(make_error(StmtErrorCode::INVALID_AST_NODE,
                                      "column definition list is missing"));
  }
  for (const ASTNode *item = list->head; item != nullptr; item = item->next) {
    if (item->type != NODE_COLUMN_DEF) {
      return std::unexpected(make_error(StmtErrorCode::INVALID_AST_NODE,
                                        "expected a column definition"));
    }
    const auto *data = reinterpret_cast<const ColumnDefNode *>(item->data);
    sql::ColumnDef column;
    column.name = sql::Identifier(data->name != nullptr ? data->name : "");
    column.type = sql::from_c(data->data_type);
    column.length = data->length;
    column.primary_key = data->is_primary_key != 0;
    // SQL 语义：PRIMARY KEY 隐含 NOT NULL（DDL 里没写也要补上）
    column.nullable = (data->nullable != 0) && !column.primary_key;
    columns.push_back(std::move(column));
  }
  return columns;
}

// ============================================================
// 各语句
// ============================================================
std::expected<sql::Query, StmtError>
StatementBuilder::build_stmt_use(ASTNode *ast) {
  const auto *data = reinterpret_cast<const DatabaseNode *>(ast->data);
  if (data->db_name == nullptr) {
    return std::unexpected(make_error(StmtErrorCode::EMPTY_STATEMENT,
                                      "USE is missing database name"));
  }
  sql::UseDatabaseQuery query;
  query.database = sql::Identifier(data->db_name);
  return sql::Query(std::move(query));
}

std::expected<sql::Query, StmtError>
StatementBuilder::build_stmt_select(ASTNode *ast) {
  const auto *data = reinterpret_cast<const SelectNode *>(ast->data);
  if (data->table == nullptr) {
    return std::unexpected(make_error(StmtErrorCode::EMPTY_STATEMENT,
                                      "SELECT is missing table name"));
  }

  sql::SelectQuery query;
  query.table = sql::Identifier(data->table);

  auto columns = build_select_columns(const_cast<ASTNode *>(data->columns));
  if (!columns.has_value()) {
    return std::unexpected(columns.error());
  }
  query.columns = std::move(*columns);

  auto where = build_condition(const_cast<ASTNode *>(data->condition));
  if (!where.has_value()) {
    return std::unexpected(where.error());
  }
  query.where = std::move(*where);

  auto order = build_order_by(const_cast<ASTNode *>(data->order_by));
  if (!order.has_value()) {
    return std::unexpected(order.error());
  }
  query.order_by = std::move(*order);

  auto limit = build_limit(const_cast<ASTNode *>(data->limit));
  if (!limit.has_value()) {
    return std::unexpected(limit.error());
  }
  query.limit = *limit;

  return sql::Query(std::move(query));
}

std::expected<sql::Query, StmtError>
StatementBuilder::build_stmt_insert(ASTNode *ast) {
  const auto *data = reinterpret_cast<const InsertNode *>(ast->data);
  if (data->table == nullptr) {
    return std::unexpected(make_error(StmtErrorCode::EMPTY_STATEMENT,
                                      "INSERT is missing table name"));
  }

  sql::InsertQuery query;
  query.table = sql::Identifier(data->table);

  auto columns = build_column_list(const_cast<ASTNode *>(data->columns));
  if (!columns.has_value()) {
    return std::unexpected(columns.error());
  }
  query.columns = std::move(*columns);

  const ASTNodeList *values = as_list(data->values);
  if (values == nullptr || values->count == 0) {
    return std::unexpected(make_error(StmtErrorCode::INVALID_AST_NODE,
                                      "INSERT is missing VALUES"));
  }

  // 语法目前只支持单行：VALUES 列表的元素就是值本身。
  // 若将来支持多行（元素是 NODE_LIST），这里也能直接处理。
  if (values->head->type == NODE_LIST) {
    for (const ASTNode *row = values->head; row != nullptr; row = row->next) {
      auto row_values = build_value_list(const_cast<ASTNode *>(row));
      if (!row_values.has_value()) {
        return std::unexpected(row_values.error());
      }
      query.values.push_back(std::move(*row_values));
    }
  } else {
    auto row_values = build_value_list(const_cast<ASTNode *>(data->values));
    if (!row_values.has_value()) {
      return std::unexpected(row_values.error());
    }
    query.values.push_back(std::move(*row_values));
  }

  return sql::Query(std::move(query));
}

std::expected<sql::Query, StmtError>
StatementBuilder::build_stmt_update(ASTNode *ast) {
  const auto *data = reinterpret_cast<const UpdateNode *>(ast->data);
  if (data->table == nullptr) {
    return std::unexpected(make_error(StmtErrorCode::EMPTY_STATEMENT,
                                      "UPDATE is missing table name"));
  }

  sql::UpdateQuery query;
  query.table = sql::Identifier(data->table);

  const ASTNodeList *assignments = as_list(data->assignments);
  if (assignments == nullptr || assignments->count == 0) {
    return std::unexpected(make_error(StmtErrorCode::EMPTY_STATEMENT,
                                      "UPDATE is missing SET assignments"));
  }
  for (const ASTNode *item = assignments->head; item != nullptr;
       item = item->next) {
    if (item->type != NODE_ASSIGNMENT) {
      return std::unexpected(make_error(StmtErrorCode::INVALID_AST_NODE,
                                        "SET list contains non-assignment"));
    }
    const auto *assign = reinterpret_cast<const AssignmentNode *>(item->data);
    auto value = build_value(const_cast<ASTNode *>(assign->value));
    if (!value.has_value()) {
      return std::unexpected(value.error());
    }
    sql::UpdateQuery::Assignment out;
    out.column =
        sql::Identifier(assign->column != nullptr ? assign->column : "");
    out.value = std::move(*value);
    query.assignments.push_back(std::move(out));
  }

  auto where = build_condition(const_cast<ASTNode *>(data->condition));
  if (!where.has_value()) {
    return std::unexpected(where.error());
  }
  query.where = std::move(*where);

  return sql::Query(std::move(query));
}

std::expected<sql::Query, StmtError>
StatementBuilder::build_stmt_delete(ASTNode *ast) {
  const auto *data = reinterpret_cast<const DeleteNode *>(ast->data);
  if (data->table == nullptr) {
    return std::unexpected(make_error(StmtErrorCode::EMPTY_STATEMENT,
                                      "DELETE is missing table name"));
  }

  sql::DeleteQuery query;
  query.table = sql::Identifier(data->table);

  auto where = build_condition(const_cast<ASTNode *>(data->condition));
  if (!where.has_value()) {
    return std::unexpected(where.error());
  }
  query.where = std::move(*where);

  return sql::Query(std::move(query));
}

std::expected<sql::Query, StmtError>
StatementBuilder::build_stmt_create_database(ASTNode *ast) {
  const auto *data = reinterpret_cast<const DatabaseNode *>(ast->data);
  if (data->db_name == nullptr) {
    return std::unexpected(make_error(StmtErrorCode::EMPTY_STATEMENT,
                                      "CREATE DATABASE is missing name"));
  }
  sql::CreateDatabaseQuery query;
  query.database = sql::Identifier(data->db_name);
  return sql::Query(std::move(query));
}

std::expected<sql::Query, StmtError>
StatementBuilder::build_stmt_drop_database(ASTNode *ast) {
  const auto *data = reinterpret_cast<const DatabaseNode *>(ast->data);
  if (data->db_name == nullptr) {
    return std::unexpected(make_error(StmtErrorCode::EMPTY_STATEMENT,
                                      "DROP DATABASE is missing name"));
  }
  sql::DropDatabaseQuery query;
  query.database = sql::Identifier(data->db_name);
  return sql::Query(std::move(query));
}

std::expected<sql::Query, StmtError>
StatementBuilder::build_stmt_create_table(ASTNode *ast) {
  const auto *data = reinterpret_cast<const CreateTableNode *>(ast->data);
  if (data->table_name == nullptr) {
    return std::unexpected(make_error(StmtErrorCode::EMPTY_STATEMENT,
                                      "CREATE TABLE is missing table name"));
  }

  sql::CreateTableQuery query;
  query.table = sql::Identifier(data->table_name);

  auto columns = build_column_defs(const_cast<ASTNode *>(data->columns));
  if (!columns.has_value()) {
    return std::unexpected(columns.error());
  }
  query.columns = std::move(*columns);

  return sql::Query(std::move(query));
}

std::expected<sql::Query, StmtError>
StatementBuilder::build_stmt_drop_table(ASTNode *ast) {
  const auto *data = reinterpret_cast<const DropTableNode *>(ast->data);
  if (data->table_name == nullptr) {
    return std::unexpected(make_error(StmtErrorCode::EMPTY_STATEMENT,
                                      "DROP TABLE is missing table name"));
  }
  sql::DropTableQuery query;
  query.table = sql::Identifier(data->table_name);
  return sql::Query(std::move(query));
}

} // namespace stmt
