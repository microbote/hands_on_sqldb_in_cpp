// session.cpp
#include "session.h"
#include "parser/parser.tab.h"
#include "parser/lex.yy.h"
#include <iostream>
#include <sstream>

extern int yyparse();
extern void yy_delete_buffer(YY_BUFFER_STATE buffer);
extern YY_BUFFER_STATE yy_scan_string(const char* str);
extern ASTNode* g_parsed_ast;

namespace session {

// ============================================================
// Cursor 实现（基于 Table Cursor）
// ============================================================
class TableCursor : public Cursor {
public:
    TableCursor(std::shared_ptr<sql::Table> table,
                std::unique_ptr<sql::Cursor> cursor)
        : table_(table), cursor_(std::move(cursor)) {}
    
    ~TableCursor() = default;
    
    std::optional<sql::Row> next() override {
        if (!cursor_) return std::nullopt;
        return cursor_->next();
    }
    
    bool has_next() const override {
        return cursor_ && cursor_->has_next();
    }
    
    void reset() override {
        if (cursor_) cursor_->reset();
    }
    
    void close() override {
        cursor_.reset();
    }
    
    std::vector<std::string> columns() const override {
        std::vector<std::string> cols;
        if (table_) {
            for (const auto& col : table_->schema().columns()) {
                cols.push_back(col.name);
            }
        }
        return cols;
    }
    
    size_t row_count() const override {
        // 无法精确获取，返回 0
        return 0;
    }

private:
    std::shared_ptr<sql::Table> table_;
    std::unique_ptr<sql::Cursor> cursor_;
};

// ============================================================
// Session 实现
// ============================================================
Session::Session(std::shared_ptr<sql::DatabaseManager> db_manager)
    : db_manager_(db_manager) {
    
    builder_ = std::make_unique<query::StatementBuilder>(db_manager);
    optimizer_ = std::make_unique<query::Optimizer>(db_manager);
    executor_ = std::make_unique<query::Executor>(db_manager);
    
    std::cout << "Session created" << std::endl;
}

Session::~Session() {
    std::cout << "Session destroyed (queries: " << query_count_ 
              << ", updates: " << update_count_ << ")" << std::endl;
}

// ============================================================
// 数据库管理
// ============================================================
bool Session::use_database(const std::string& db_name) {
    if (!db_manager_->database_exists(db_name)) {
        std::cerr << "Database '" << db_name << "' does not exist" << std::endl;
        return false;
    }
    
    current_db_ = db_name;
    builder_->set_current_db(current_db_);
    
    std::cout << "Using database: " << db_name << std::endl;
    return true;
}

bool Session::create_database(const std::string& db_name) {
    return db_manager_->create_database(db_name);
}

bool Session::drop_database(const std::string& db_name) {
    if (db_name == current_db_) {
        current_db_.clear();
        builder_->set_current_db("");
    }
    return db_manager_->drop_database(db_name);
}

bool Session::create_table(const std::string& table_name, const sql::TableSchema& schema) {
    auto db = get_database();
    if (!db) return false;
    return db->create_table(schema);
}

bool Session::drop_table(const std::string& table_name) {
    auto db = get_database();
    if (!db) return false;
    return db->drop_table(table_name);
}

std::vector<std::string> Session::list_tables() const {
    auto db = get_database();
    if (!db) return {};
    return db->list_tables();
}

// ============================================================
// SQL 执行（核心入口）
// ============================================================
ExecResult Session::execute(const std::string& sql) {
    if (current_db_.empty()) {
        return ExecResult(false, "No database selected. Use USE <database> first.");
    }
    
    std::cout << "Executing: " << sql << std::endl;
    
    // Step 1: 解析 SQL → AST
    ASTNode* ast = parse_sql(sql);
    if (!ast) {
        return ExecResult(false, "Parse error");
    }
    
    // Step 2: AST → Statement
    auto stmt = build_statement(ast);
    if (!stmt) {
        free_ast(ast);
        return ExecResult(false, "Statement build error: " + builder_->get_error());
    }
    
    // Step 3: 根据语句类型执行
    ExecResult result;
    switch (stmt->type()) {
        case query::StatementType::SELECT:
            result = execute_select(static_cast<query::SelectStatement*>(stmt.get()));
            query_count_++;
            break;
            
        case query::StatementType::INSERT:
            result = execute_insert(static_cast<query::InsertStatement*>(stmt.get()));
            update_count_++;
            break;
            
        case query::StatementType::UPDATE:
            result = execute_update(static_cast<query::UpdateStatement*>(stmt.get()));
            update_count_++;
            break;
            
        case query::StatementType::DELETE:
            result = execute_delete(static_cast<query::DeleteStatement*>(stmt.get()));
            update_count_++;
            break;
            
        case query::StatementType::USE:
            result = execute_use(static_cast<query::UseStatement*>(stmt.get()));
            break;
            
        default:
            result = execute_ddl(stmt.get());
            break;
    }
    
    free_ast(ast);
    return result;
}

std::unique_ptr<Cursor> Session::query(const std::string& sql) {
    auto result = execute(sql);
    if (result.success && result.cursor) {
        return std::move(result.cursor);
    }
    return nullptr;
}

int64_t Session::update(const std::string& sql) {
    auto result = execute(sql);
    return result.success ? result.affected_rows : -1;
}

bool Session::ddl(const std::string& sql) {
    auto result = execute(sql);
    return result.success;
}

// ============================================================
// 内部流程
// ============================================================
ASTNode* Session::parse_sql(const std::string& sql) {
    YY_BUFFER_STATE buffer = yy_scan_string(sql.c_str());
    int result = yyparse();
    yy_delete_buffer(buffer);
    
    if (result != 0 || !g_parsed_ast) {
        return nullptr;
    }
    
    ASTNode* ast = g_parsed_ast;
    g_parsed_ast = nullptr;
    return ast;
}

std::unique_ptr<query::Statement> Session::build_statement(ASTNode* ast) {
    builder_->clear_error();
    auto stmt = builder_->build(ast);
    if (builder_->has_error()) {
        std::cerr << "Statement build error: " << builder_->get_error() << std::endl;
        return nullptr;
    }
    return stmt;
}

std::unique_ptr<query::ExecutionPlan> Session::optimize(query::Statement* stmt) {
    return optimizer_->optimize(stmt, current_db_);
}

ExecResult Session::execute_plan(query::ExecutionPlan* plan) {
    auto result = executor_->execute(plan);
    if (!result.success) {
        return ExecResult(false, "Execution failed: " + result.message);
    }
    return ExecResult::with_affected(result.affected_rows);
}

// ============================================================
// 具体语句执行
// ============================================================
ExecResult Session::execute_select(query::SelectStatement* stmt) {
    // 1. 验证表存在
    auto schema = get_table_schema(stmt->table_name);
    if (schema.name().empty()) {
        return ExecResult(false, "Table '" + stmt->table_name + "' not found");
    }
    
    // 2. 优化：Statement → Plan
    auto plan = optimize(stmt);
    if (!plan) {
        return ExecResult(false, "Optimization failed");
    }
    
    std::cout << "  Plan: " << plan->to_string() << std::endl;
    
    // 3. 执行：Plan → Cursor
    auto table = get_table(stmt->table_name);
    if (!table) {
        return ExecResult(false, "Table '" + stmt->table_name + "' not found");
    }
    
    // 执行计划，获取游标
    // 注意：这里需要 Executor 支持返回 Cursor
    // 当前 executor_->execute 返回 ExecResult，包含 rows
    // 我们需要修改 Executor 支持游标模式
    
    // 临时方案：直接获取所有行，包装为游标
    auto exec_result = executor_->execute(plan.get());
    if (!exec_result.success) {
        return ExecResult(false, "Execution failed: " + exec_result.message);
    }
    
    // 创建游标（将 rows 包装为游标）
    // TODO: 实现真正的游标（流式）
    class RowsCursor : public Cursor {
    public:
        RowsCursor(std::vector<sql::Row>&& rows) 
            : rows_(std::move(rows)), pos_(0) {}
        
        std::optional<sql::Row> next() override {
            if (pos_ >= rows_.size()) return std::nullopt;
            return rows_[pos_++];
        }
        
        bool has_next() const override {
            return pos_ < rows_.size();
        }
        
        void reset() override { pos_ = 0; }
        void close() override { rows_.clear(); }
        std::vector<std::string> columns() const override { return {}; }
        size_t row_count() const override { return rows_.size(); }
        
    private:
        std::vector<sql::Row> rows_;
        size_t pos_;
    };
    
    return ExecResult::with_cursor(
        std::make_unique<RowsCursor>(std::move(exec_result.rows))
    );
}

ExecResult Session::execute_insert(query::InsertStatement* stmt) {
    auto plan = optimize(stmt);
    if (!plan) {
        return ExecResult(false, "Optimization failed");
    }
    
    auto result = executor_->execute(plan.get());
    if (!result.success) {
        return ExecResult(false, "Insert failed: " + result.message);
    }
    
    return ExecResult::with_affected(result.affected_rows);
}

ExecResult Session::execute_update(query::UpdateStatement* stmt) {
    auto plan = optimize(stmt);
    if (!plan) {
        return ExecResult(false, "Optimization failed");
    }
    
    auto result = executor_->execute(plan.get());
    if (!result.success) {
        return ExecResult(false, "Update failed: " + result.message);
    }
    
    return ExecResult::with_affected(result.affected_rows);
}

ExecResult Session::execute_delete(query::DeleteStatement* stmt) {
    auto plan = optimize(stmt);
    if (!plan) {
        return ExecResult(false, "Optimization failed");
    }
    
    auto result = executor_->execute(plan.get());
    if (!result.success) {
        return ExecResult(false, "Delete failed: " + result.message);
    }
    
    return ExecResult::with_affected(result.affected_rows);
}

ExecResult Session::execute_use(query::UseStatement* stmt) {
    if (use_database(stmt->database_name)) {
        return ExecResult(true, "Switched to " + stmt->database_name);
    }
    return ExecResult(false, "Failed to switch database");
}

ExecResult Session::execute_ddl(query::Statement* stmt) {
    // TODO: 实现 DDL 执行
    return ExecResult(false, "DDL execution not yet implemented");
}

// ============================================================
// 辅助方法
// ============================================================
sql::TableSchema Session::get_table_schema(const std::string& table_name) {
    auto db = get_database();
    if (!db) return sql::TableSchema();
    auto table = db->get_table(table_name);
    if (!table) return sql::TableSchema();
    return table->schema();
}

std::shared_ptr<sql::Table> Session::get_table(const std::string& table_name) {
    auto db = get_database();
    if (!db) return nullptr;
    return db->get_table(table_name);
}

std::shared_ptr<sql::Database> Session::get_database() {
    if (current_db_.empty()) {
        std::cerr << "No database selected" << std::endl;
        return nullptr;
    }
    return db_manager_->open_database(current_db_);
}

void Session::reset_parser_state() {
    // 清理全局解析器状态
}

// ============================================================
// TableSchemaBuilder 实现
// ============================================================
TableSchemaBuilder::TableSchemaBuilder(const std::string& name) {
    schema_.set_name(name);
}

TableSchemaBuilder& TableSchemaBuilder::column(const std::string& name, 
                                               sql::DataType type,
                                               bool nullable, bool pk) {
    schema_.add_column(name, type, pk, nullable);
    return *this;
}

TableSchemaBuilder& TableSchemaBuilder::primary_key(const std::string& name, 
                                                    sql::DataType type) {
    schema_.add_column(name, type, true, false);
    return *this;
}

TableSchemaBuilder& TableSchemaBuilder::not_null(const std::string& name, 
                                                 sql::DataType type) {
    schema_.add_column(name, type, false, false);
    return *this;
}

TableSchemaBuilder& TableSchemaBuilder::nullable(const std::string& name, 
                                                 sql::DataType type) {
    schema_.add_column(name, type, false, true);
    return *this;
}

} // namespace session