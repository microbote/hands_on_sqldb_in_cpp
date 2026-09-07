
#include "stmt_ddl.h"

namespace stmt {
// ============================================================
// CreateDatabaseStatement
// ============================================================
CreateDatabaseStatement::CreateDatabaseStatement(const std::string &db)
    : database_name(db) {
  set_valid(!db.empty());
}

StatementType CreateDatabaseStatement::type() const {
  return StatementType::CREATE_DATABASE;
}

std::string CreateDatabaseStatement::to_string() const {
  return "CREATE DATABASE " + database_name;
}

// ============================================================
// DropDatabaseStatement
// ============================================================
DropDatabaseStatement::DropDatabaseStatement(const std::string &db)
    : database_name(db) {
  set_valid(!db.empty());
}

StatementType DropDatabaseStatement::type() const {
  return StatementType::DROP_DATABASE;
}

std::string DropDatabaseStatement::to_string() const {
  return "DROP DATABASE " + database_name;
}

// ============================================================
// CreateTableStatement
// ============================================================
CreateTableStatement::CreateTableStatement(const std::string &table)
    : table_name(table) {
  set_valid(!table.empty());
}

StatementType CreateTableStatement::type() const {
  return StatementType::CREATE_TABLE;
}

std::string CreateTableStatement::to_string() const {
  std::string result = "CREATE TABLE " + table_name + " (";
  const auto &cols = schema.columns();
  for (size_t i = 0; i < cols.size(); ++i) {
    if (i > 0) {
      result += ", ";
}
    result += cols[i].name + " " + data_type_to_string(cols[i].type);
    if (cols[i].primary_key) {
      result += " PRIMARY KEY";
}
    if (!cols[i].nullable) {
      result += " NOT NULL";
}
  }
  result += ")";
  return result;
}

// ============================================================
// DropTableStatement
// ============================================================
DropTableStatement::DropTableStatement(const std::string &table)
    : table_name(table) {
  set_valid(!table.empty());
}

StatementType DropTableStatement::type() const {
  return StatementType::DROP_TABLE;
}

std::string DropTableStatement::to_string() const {
  return "DROP TABLE " + table_name;
}

} // namespace stmt