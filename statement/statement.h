// query/statement.h
#ifndef QUERY_STATEMENT_H
#define QUERY_STATEMENT_H

#include <memory>
#include <string>
#include <vector>

#include "condition.h"
#include "relation/sql_relation.h"

namespace stmt {

enum class StatementCatalog : std::uint8_t {
  DDL,
  DML,
  CTRL,
  UNKNOWN
};

inline const char * statement_catalog_to_string(StatementCatalog type){
  switch(type){
    case StatementCatalog::DDL: return "DDL";
    case StatementCatalog::DML: return "DML";
    case StatementCatalog::CTRL: return "Control";
    default: return "UNKNOWN";
  }
}


// ============================================================
// 语句类型枚举
// ============================================================
enum class StatementType : std::uint8_t {
    // DDL
    CREATE_DATABASE,
    DROP_DATABASE,
    CREATE_TABLE,
    DROP_TABLE,
    // DML
    SELECT,
    INSERT,
    UPDATE,
    DELETE,
    // Control
    USE,
    UNKNOWN
};

inline const char* statement_type_to_string(StatementType type) {
    switch (type) {
        case StatementType::CREATE_DATABASE: return "CREATE_DATABASE";
        case StatementType::DROP_DATABASE:   return "DROP_DATABASE";
        case StatementType::CREATE_TABLE:    return "CREATE_TABLE";
        case StatementType::DROP_TABLE:      return "DROP_TABLE";
        case StatementType::SELECT:          return "SELECT";
        case StatementType::INSERT:          return "INSERT";
        case StatementType::UPDATE:          return "UPDATE";
        case StatementType::DELETE:          return "DELETE";
        case StatementType::USE:             return "USE";
        default:                             return "UNKNOWN";
    }
}

// ============================================================
// 语句基类
// ============================================================
class Statement {
public:
    virtual ~Statement() = default;
    
    virtual StatementCatalog catalog() const = 0;
    virtual StatementType type() const = 0;
    virtual std::string to_string() const = 0;
    
    virtual bool is_valid() const { return valid_; }
    void make_valid() { valid_ = true; }
    void make_invalid() { valid_ = false; }
    void set_valid(bool valid) { valid_= valid;}
    
    virtual bool is_ddl() const { return false; }
    virtual bool is_dml() const { return false; }
    virtual bool is_control() const { return false; }

private:
    bool valid_ = true;
};

// ============================================================
// DDL 语句基类（Data Definition Language）
// ============================================================
class DDLStatement : public Statement {
public:
    bool is_ddl() const override { return true; }
};

// ============================================================
// DML 语句基类（Data Manipulation Language）
// ============================================================
class DMLStatement : public Statement {
public:
    bool is_dml() const override { return true; }
    virtual const std::string& table_name() const = 0;
};

// ============================================================
// 控制语句基类（Session Control）
// ============================================================
class ControlStatement : public Statement {
public:
    bool is_control() const override { return true; }
};

// ============================================================
// USE 语句（Control）
// ============================================================
class UseStatement : public ControlStatement {
public:
    std::string database_name;

    explicit UseStatement(const std::string& db);
    StatementType type() const override;
    std::string to_string() const override;
};



// ============================================================
// SELECT 语句（DML）
// ============================================================
class SelectStatement : public DMLStatement {
public:
    std::string table_name_;
    std::vector<std::string> columns_;
    std::unique_ptr<ConditionExpr> condition_;

    explicit SelectStatement(const std::string& table);
    StatementType type() const override;
    std::string to_string() const override;
    const std::string& table_name() const override { return table_name_; }

    // 便捷方法
    bool is_select_all() const;
};

// ============================================================
// INSERT 语句（DML）
// ============================================================
class InsertStatement : public DMLStatement {
public:
    std::string table_name_;
    std::vector<std::string> columns_;
    std::vector<sql::Value> values_;

    explicit InsertStatement(const std::string& table);
    StatementType type() const override;
    std::string to_string() const override;
    const std::string& table_name() const override { return table_name_; }

    bool has_columns() const { return !columns_.empty(); }
    bool has_values() const { return !values_.empty(); }
    size_t column_count() const { return columns_.size(); }
    size_t value_count() const { return values_.size(); }
};

// ============================================================
// UPDATE 语句（DML）
// ============================================================
class UpdateStatement : public DMLStatement {
public:
    std::string table_name_;
    std::vector<std::pair<std::string, sql::Value>> assignments_;
    std::unique_ptr<ConditionExpr> condition_;

    explicit UpdateStatement(const std::string& table);
    StatementType type() const override;
    std::string to_string() const override;
    const std::string& table_name() const override { return table_name_; }

    bool has_assignments() const { return !assignments_.empty(); }
    bool has_condition() const { return condition_ != nullptr; }
};

// ============================================================
// DELETE 语句（DML）
// ============================================================
class DeleteStatement : public DMLStatement {
public:
    std::string table_name_;
    std::unique_ptr<ConditionExpr> condition_;

    explicit DeleteStatement(const std::string& table);
    StatementType type() const override;
    std::string to_string() const override;
    const std::string& table_name() const override { return table_name_; }

    bool has_condition() const { return condition_ != nullptr; }
};

} // namespace query

#endif // QUERY_STATEMENT_H