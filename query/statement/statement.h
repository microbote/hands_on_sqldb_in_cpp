// statement.h
#ifndef QUERY_STATEMENT_H
#define QUERY_STATEMENT_H

#include <memory>
#include <string>
#include <vector>

#include "relation/sql_relation.h"
#include "condition.h"

namespace query {

// ============================================================
// 语句类型
// ============================================================
enum class StatementType { USE, SELECT, INSERT, UPDATE, DELETE, UNKNOWN };

// ============================================================
// 语句基类
// ============================================================
class Statement {
 public:
  virtual ~Statement() = default;
  virtual StatementType type() const = 0;
  virtual std::string to_string() const = 0;
  virtual bool is_valid() const { return valid_; }

 protected:
  bool valid_ = true;
};

// ============================================================
// USE 语句
// ============================================================
class UseStatement : public Statement {
 public:
  std::string database_name;

  UseStatement(const std::string& db) : database_name(db) {}
  StatementType type() const override { return StatementType::USE; }
  std::string to_string() const override { return "USE " + database_name; }
};

// ============================================================
// SELECT 语句
// ============================================================
class SelectStatement : public Statement {
 public:
  std::string table_name;
  std::vector<std::string> columns;          // 空表示 SELECT *
  std::unique_ptr<ConditionExpr> condition;  // WHERE 条件

  SelectStatement(const std::string& table) : table_name(table) {}
  StatementType type() const override { return StatementType::SELECT; }

  std::string to_string() const override {
    std::string result = "SELECT ";
    if (columns.empty() || (columns.size() == 1 && columns[0] == "*")) {
      result += "*";
    } else {
      for (size_t i = 0; i < columns.size(); ++i) {
        if (i > 0) result += ", ";
        result += columns[i];
      }
    }
    result += " FROM " + table_name;
    if (condition) {
      result += " WHERE " + condition->to_string();
    }
    return result;
  }
};

// ============================================================
// INSERT 语句
// ============================================================
class InsertStatement : public Statement {
 public:
  std::string table_name;
  std::vector<std::string> columns;
  std::vector<sql::Value> values;

  InsertStatement(const std::string& table) : table_name(table) {}
  StatementType type() const override { return StatementType::INSERT; }

  std::string to_string() const override {
    std::string result = "INSERT INTO " + table_name;
    if (!columns.empty()) {
      result += " (";
      for (size_t i = 0; i < columns.size(); ++i) {
        if (i > 0) result += ", ";
        result += columns[i];
      }
      result += ")";
    }
    result += " VALUES (";
    for (size_t i = 0; i < values.size(); ++i) {
      if (i > 0) result += ", ";
      result += values[i].to_string();
    }
    result += ")";
    return result;
  }
};

// ============================================================
// UPDATE 语句
// ============================================================
class UpdateStatement : public Statement {
 public:
  std::string table_name;
  std::vector<std::pair<std::string, sql::Value>> assignments;
  std::unique_ptr<ConditionExpr> condition;

  UpdateStatement(const std::string& table) : table_name(table) {}
  StatementType type() const override { return StatementType::UPDATE; }

  std::string to_string() const override {
    std::string result = "UPDATE " + table_name + " SET ";
    for (size_t i = 0; i < assignments.size(); ++i) {
      if (i > 0) result += ", ";
      result +=
          assignments[i].first + " = " + assignments[i].second.to_string();
    }
    if (condition) {
      result += " WHERE " + condition->to_string();
    }
    return result;
  }
};

// ============================================================
// DELETE 语句
// ============================================================
class DeleteStatement : public Statement {
 public:
  std::string table_name;
  std::unique_ptr<ConditionExpr> condition;

  DeleteStatement(const std::string& table) : table_name(table) {}
  StatementType type() const override { return StatementType::DELETE; }

  std::string to_string() const override {
    std::string result = "DELETE FROM " + table_name;
    if (condition) {
      result += " WHERE " + condition->to_string();
    }
    return result;
  }
};

}  // namespace query

#endif  // QUERY_STATEMENT_H