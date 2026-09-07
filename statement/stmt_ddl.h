#ifndef STMT_DDL_H
#define STMT_DDL_H

#include "statement.h"

namespace stmt{
// ============================================================
// CREATE DATABASE 语句（DDL）
// ============================================================
class CreateDatabaseStatement : public DDLStatement {
  public:
      std::string database_name;
  
      explicit CreateDatabaseStatement(const std::string& db);
      StatementType type() const override;
      std::string to_string() const override;
  };
  
  // ============================================================
  // DROP DATABASE 语句（DDL）
  // ============================================================
  class DropDatabaseStatement : public DDLStatement {
  public:
      std::string database_name;
  
      explicit DropDatabaseStatement(const std::string& db);
      StatementType type() const override;
      std::string to_string() const override;
  };
  
  // ============================================================
  // CREATE TABLE 语句（DDL）
  // ============================================================
  class CreateTableStatement : public DDLStatement {
  public:
      std::string table_name;
      sql::TableSchema schema;
  
      explicit CreateTableStatement(const std::string& table);
      StatementType type() const override;
      std::string to_string() const override;
  };
  
  // ============================================================
  // DROP TABLE 语句（DDL）
  // ============================================================
  class DropTableStatement : public DDLStatement {
  public:
      std::string table_name;
  
      explicit DropTableStatement(const std::string& table);
      StatementType type() const override;
      std::string to_string() const override;
  };


}

#endif