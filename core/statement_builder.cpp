// statement_builder.cpp
#include "statement_builder.h"

#include <iostream>

namespace sql {

StatementBuilder::StatementBuilder(
    std::shared_ptr<storage::StorageEngine> engine)
    : engine_(engine) {}

std::unique_ptr<Statement> StatementBuilder::build(ASTNode* ast) {
  error_.clear();

  if (!ast) {
    error_ = "AST is null";
    return nullptr;
  }

  switch (ast->type) {
    case NODE_USE:
      return build_use(ast);
    case NODE_SELECT:
      return build_select(ast);
    case NODE_INSERT:
      return build_insert(ast);
    case NODE_UPDATE:
      return build_update(ast);
    case NODE_DELETE:
      return build_delete(ast);
    default:
      error_ = "Unknown AST node type: " + std::to_string(ast->type);
      return nullptr;
  }
}

// ===== USE 语句 =====
std::unique_ptr<UseStatement> StatementBuilder::build_use(ASTNode* node) {
  if (!node->str_val) {
    error_ = "USE statement missing database name";
    return nullptr;
  }
  return std::make_unique<UseStatement>(node->str_val);
}

// ===== SELECT 语句 =====
std::unique_ptr<SelectStatement> StatementBuilder::build_select(ASTNode* node) {
  if (!node->str_val) {
    error_ = "SELECT statement missing table name";
    return nullptr;
  }

  std::string table_name = node->str_val;

  // ✅ 验证表是否存在
  if (!validate_table(table_name)) {
    error_ = "Table '" + table_name + "' does not exist";
    return nullptr;
  }

  auto stmt = std::make_unique<SelectStatement>(table_name);

  // 构建列列表
  if (node->left && node->left->type == NODE_LIST) {
    stmt->columns = build_column_list(node->left, table_name);
    if (has_error()) return nullptr;
  }

  // 构建 WHERE 条件
  if (node->right) {
    stmt->conditions = build_conditions(node->right, table_name);
    if (has_error()) return nullptr;
  }

  stmt->valid_ = true;
  return stmt;
}

// ===== INSERT 语句 =====
std::unique_ptr<InsertStatement> StatementBuilder::build_insert(ASTNode* node) {
  if (!node->str_val) {
    error_ = "INSERT statement missing table name";
    return nullptr;
  }

  std::string table_name = node->str_val;

  // ✅ 验证表是否存在
  if (!validate_table(table_name)) {
    error_ = "Table '" + table_name + "' does not exist";
    return nullptr;
  }

  auto stmt = std::make_unique<InsertStatement>(table_name);

  // 构建列列表
  if (node->left && node->left->type == NODE_LIST) {
    stmt->columns = build_column_list(node->left, table_name);
    if (has_error()) return nullptr;
  }

  // 构建值列表
  if (node->right && node->right->type == NODE_LIST) {
    stmt->values = build_value_list(node->right);
    if (has_error()) return nullptr;
  }

  // ✅ 验证列数和值数匹配
  if (!stmt->columns.empty() && stmt->columns.size() != stmt->values.size()) {
    error_ = "Column count (" + std::to_string(stmt->columns.size()) +
             ") does not match value count (" +
             std::to_string(stmt->values.size()) + ")";
    return nullptr;
  }

  // ✅ 验证值类型匹配
  auto schema = engine_->get_table_schema(table_name);
  for (size_t i = 0; i < stmt->columns.size(); ++i) {
    auto* col = schema.get_column(stmt->columns[i]);
    if (col) {
      if (!validate_value_type(stmt->values[i], col->type)) {
        error_ = "Type mismatch for column '" + stmt->columns[i] + "'";
        return nullptr;
      }
    }
  }

  stmt->valid_ = true;
  return stmt;
}

// ===== UPDATE 语句 =====
std::unique_ptr<UpdateStatement> StatementBuilder::build_update(ASTNode* node) {
  if (!node->str_val) {
    error_ = "UPDATE statement missing table name";
    return nullptr;
  }

  std::string table_name = node->str_val;

  // ✅ 验证表是否存在
  if (!validate_table(table_name)) {
    error_ = "Table '" + table_name + "' does not exist";
    return nullptr;
  }

  auto stmt = std::make_unique<UpdateStatement>(table_name);

  // 构建赋值列表
  if (node->left && node->left->type == NODE_LIST) {
    stmt->assignments = build_assignments(node->left, table_name);
    if (has_error()) return nullptr;
  }

  // 构建 WHERE 条件
  if (node->right) {
    stmt->conditions = build_conditions(node->right, table_name);
    if (has_error()) return nullptr;
  }

  stmt->valid_ = true;
  return stmt;
}

// ===== DELETE 语句 =====
std::unique_ptr<DeleteStatement> StatementBuilder::build_delete(ASTNode* node) {
  if (!node->str_val) {
    error_ = "DELETE statement missing table name";
    return nullptr;
  }

  std::string table_name = node->str_val;

  // ✅ 验证表是否存在
  if (!validate_table(table_name)) {
    error_ = "Table '" + table_name + "' does not exist";
    return nullptr;
  }

  auto stmt = std::make_unique<DeleteStatement>(table_name);

  // 构建 WHERE 条件
  if (node->right) {
    stmt->conditions = build_conditions(node->right, table_name);
    if (has_error()) return nullptr;
  }

  stmt->valid_ = true;
  return stmt;
}

// ===== 验证辅助方法 =====
bool StatementBuilder::validate_table(const std::string& name) {
  if (!engine_) return false;
  return engine_->table_exists(name);
}

bool StatementBuilder::validate_column(const std::string& table,
                                       const std::string& column) {
  if (!engine_) return false;
  auto schema = engine_->get_table_schema(table);
  return schema.get_column_index(column) >= 0;
}

bool StatementBuilder::validate_value_type(const Value& value,
                                           DataType expected) {
  return value.type == expected || value.type == DataType::NULL_TYPE;
}

bool StatementBuilder::validate_conditions(
    const std::vector<Condition>& conditions, const std::string& table) {
  for (const auto& cond : conditions) {
    if (!validate_column(table, cond.column)) {
      error_ =
          "Column '" + cond.column + "' not found in table '" + table + "'";
      return false;
    }
  }
  return true;
}

// ===== 转换辅助方法 =====
std::vector<Condition> StatementBuilder::build_conditions(
    ASTNode* node, const std::string& table) {
  std::vector<Condition> conditions;

  if (!node) return conditions;

  if (node->type == NODE_COMPARE) {
    if (node->left && node->left->type == NODE_IDENT) {
      std::string col_name = node->left->str_val ? node->left->str_val : "";

      // ✅ 验证列是否存在
      if (!validate_column(table, col_name)) {
        error_ = "Column '" + col_name + "' not found in condition";
        return conditions;
      }

      Condition cond;
      cond.column = col_name;
      cond.op = get_compare_op(node->op);
      cond.value = ast_to_value(node->right);
      conditions.push_back(cond);
    }
  } else if (node->type == NODE_BINARY_OP) {
    auto left = build_conditions(node->left, table);
    if (has_error()) return conditions;
    auto right = build_conditions(node->right, table);
    if (has_error()) return conditions;

    conditions.insert(conditions.end(), left.begin(), left.end());
    conditions.insert(conditions.end(), right.begin(), right.end());
  }

  return conditions;
}

std::vector<std::string> StatementBuilder::build_column_list(
    ASTNode* node, const std::string& table) {
  std::vector<std::string> columns;

  if (!node || node->type != NODE_LIST) return columns;

  ASTNodeList* curr = node->list;
  while (curr) {
    if (curr->node && curr->node->type == NODE_IDENT) {
      std::string col_name = curr->node->str_val ? curr->node->str_val : "";

      // ✅ 验证列是否存在（除了 *）
      if (col_name != "*" && !validate_column(table, col_name)) {
        error_ = "Column '" + col_name + "' not found in table '" + table + "'";
        return columns;
      }
      columns.push_back(col_name);
    }
    curr = curr->next;
  }

  return columns;
}

std::vector<Value> StatementBuilder::build_value_list(ASTNode* node) {
  std::vector<Value> values;

  if (!node || node->type != NODE_LIST) return values;

  ASTNodeList* curr = node->list;
  while (curr) {
    values.push_back(ast_to_value(curr->node));
    curr = curr->next;
  }

  return values;
}

std::vector<std::pair<std::string, Value>> StatementBuilder::build_assignments(
    ASTNode* node, const std::string& table) {
  std::vector<std::pair<std::string, Value>> assignments;

  if (!node || node->type != NODE_LIST) return assignments;

  ASTNodeList* curr = node->list;
  while (curr) {
    if (curr->node && curr->node->type == NODE_ASSIGNMENT) {
      ASTNode* assign = curr->node;
      if (assign->left && assign->left->type == NODE_IDENT) {
        std::string col_name =
            assign->left->str_val ? assign->left->str_val : "";

        // ✅ 验证列是否存在
        if (!validate_column(table, col_name)) {
          error_ = "Column '" + col_name + "' not found in assignment";
          return assignments;
        }

        Value val = ast_to_value(assign->right);
        assignments.push_back({col_name, val});
      }
    }
    curr = curr->next;
  }

  return assignments;
}

Value StatementBuilder::ast_to_value(ASTNode* node) {
  if (!node) return Value();

  switch (node->type) {
    case NODE_NUMBER:
      return Value(node->num_val);
    case NODE_STRING:
      return Value(node->str_val ? node->str_val : "");
    case NODE_IDENT:
      return Value(node->str_val ? node->str_val : "");
    default:
      return Value();
  }
}

CompareOp StatementBuilder::get_compare_op(const char* op) {
  if (!op) return CompareOp::EQ;

  std::string op_str(op);
  if (op_str == "=") return CompareOp::EQ;
  if (op_str == "!=") return CompareOp::NE;
  if (op_str == ">") return CompareOp::GT;
  if (op_str == ">=") return CompareOp::GE;
  if (op_str == "<") return CompareOp::LT;
  if (op_str == "<=") return CompareOp::LE;
  if (op_str == "LIKE") return CompareOp::LIKE;

  return CompareOp::EQ;
}

}  // namespace sql