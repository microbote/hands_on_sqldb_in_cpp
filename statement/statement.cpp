// query/statement.cpp
#include "statement.h"

namespace stmt {

// ============================================================
// UseStatement
// ============================================================
UseStatement::UseStatement(const std::string& db)
    : database_name(db) {
    valid_ = !db.empty();
}

StatementType UseStatement::type() const {
    return StatementType::USE;
}

std::string UseStatement::to_string() const {
    return "USE " + database_name;
}


// ============================================================
// SelectStatement
// ============================================================
SelectStatement::SelectStatement(const std::string& table)
    : table_name_(table) {
    valid_ = !table.empty();
}

StatementType SelectStatement::type() const {
    return StatementType::SELECT;
}

std::string SelectStatement::to_string() const {
    std::string result = "SELECT ";
    if (columns_.empty() || (columns_.size() == 1 && columns_[0] == "*")) {
        result += "*";
    } else {
        for (size_t i = 0; i < columns_.size(); ++i) {
            if (i > 0) result += ", ";
            result += columns_[i];
        }
    }
    result += " FROM " + table_name_;
    if (condition_) {
        result += " WHERE " + condition_->to_string();
    }
    return result;
}

bool SelectStatement::is_select_all() const {
    return columns_.empty() || (columns_.size() == 1 && columns_[0] == "*");
}

// ============================================================
// InsertStatement
// ============================================================
InsertStatement::InsertStatement(const std::string& table)
    : table_name_(table) {
    valid_ = !table.empty();
}

StatementType InsertStatement::type() const {
    return StatementType::INSERT;
}

std::string InsertStatement::to_string() const {
    std::string result = "INSERT INTO " + table_name_;
    if (!columns_.empty()) {
        result += " (";
        for (size_t i = 0; i < columns_.size(); ++i) {
            if (i > 0) result += ", ";
            result += columns_[i];
        }
        result += ")";
    }
    result += " VALUES (";
    for (size_t i = 0; i < values_.size(); ++i) {
        if (i > 0) result += ", ";
        result += values_[i].to_string();
    }
    result += ")";
    return result;
}

// ============================================================
// UpdateStatement
// ============================================================
UpdateStatement::UpdateStatement(const std::string& table)
    : table_name_(table) {
    valid_ = !table.empty();
}

StatementType UpdateStatement::type() const {
    return StatementType::UPDATE;
}

std::string UpdateStatement::to_string() const {
    std::string result = "UPDATE " + table_name_ + " SET ";
    for (size_t i = 0; i < assignments_.size(); ++i) {
        if (i > 0) result += ", ";
        result += assignments_[i].first + " = " + assignments_[i].second.to_string();
    }
    if (condition_) {
        result += " WHERE " + condition_->to_string();
    }
    return result;
}

// ============================================================
// DeleteStatement
// ============================================================
DeleteStatement::DeleteStatement(const std::string& table)
    : table_name_(table) {
    valid_ = !table.empty();
}

StatementType DeleteStatement::type() const {
    return StatementType::DELETE;
}

std::string DeleteStatement::to_string() const {
    std::string result = "DELETE FROM " + table_name_;
    if (condition_) {
        result += " WHERE " + condition_->to_string();
    }
    return result;
}

} // namespace query