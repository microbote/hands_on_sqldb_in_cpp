// session.h
#ifndef SESSION_H
#define SESSION_H

#include <memory>
#include <string>
#include <vector>
#include <optional>
#include <unordered_map>
#include <iostream>

#include "relation/database_manager.h"
#include "query/statement/statement_builder.h"
#include "query/planner/optimizer.h"
#include "query/executor/executor.h"
#include "query/planner/plan.h"
#include "query/statement/condition.h"

namespace session {

// ============================================================
// 游标接口（Volcano 模型）
// ============================================================
class Cursor {
public:
    virtual ~Cursor() = default;
    
    // 获取下一行
    virtual std::optional<sql::Row> next() = 0;
    
    // 检查是否还有数据
    virtual bool has_next() const = 0;
    
    // 重置游标
    virtual void reset() = 0;
    
    // 关闭游标
    virtual void close() = 0;
    
    // 获取列信息
    virtual std::vector<std::string> columns() const = 0;
    
    // 获取行数
    virtual size_t row_count() const { return 0; }
};

// ============================================================
// 执行结果
// ============================================================
struct ExecResult {
    bool success;
    std::string message;
    std::unique_ptr<Cursor> cursor;  // SELECT 返回
    int64_t affected_rows;            // INSERT/UPDATE/DELETE 返回
    
    ExecResult() : success(false), affected_rows(0) {}
    ExecResult(bool s, const std::string& msg = "") 
        : success(s), message(msg), affected_rows(0) {}
    
    // 创建 SELECT 结果（带游标）
    static ExecResult with_cursor(std::unique_ptr<Cursor> c) {
        ExecResult result;
        result.success = true;
        result.cursor = std::move(c);
        return result;
    }
    
    // 创建修改结果（带影响行数）
    static ExecResult with_affected(int64_t n) {
        ExecResult result;
        result.success = true;
        result.affected_rows = n;
        return result;
    }
};

// ============================================================
// Session（汇总所有组件）
// ============================================================
class Session {
public:
    explicit Session(std::shared_ptr<sql::DatabaseManager> db_manager);
    ~Session();

    // ----- 数据库管理 -----
    bool use_database(const std::string& db_name);
    std::string current_database() const { return current_db_; }
    
    // 创建数据库
    bool create_database(const std::string& db_name);
    
    // 删除数据库
    bool drop_database(const std::string& db_name);
    
    // ----- 表管理 -----
    bool create_table(const std::string& table_name, const sql::TableSchema& schema);
    bool drop_table(const std::string& table_name);
    std::vector<std::string> list_tables() const;
    
    // ----- SQL 执行（核心入口） -----
    // 执行 SQL，返回结果
    ExecResult execute(const std::string& sql);
    
    // 执行查询（SELECT），返回游标
    std::unique_ptr<Cursor> query(const std::string& sql);
    
    // 执行更新（INSERT/UPDATE/DELETE），返回影响行数
    int64_t update(const std::string& sql);
    
    // 执行 DDL，返回是否成功
    bool ddl(const std::string& sql);

    // ----- 统计信息 -----
    size_t query_count() const { return query_count_; }
    size_t update_count() const { return update_count_; }

private:
    // ============================================================
    // 内部执行流程
    // ============================================================
    
    // Step 1: 解析 SQL → AST
    ASTNode* parse_sql(const std::string& sql);
    
    // Step 2: AST → Statement
    std::unique_ptr<query::Statement> build_statement(ASTNode* ast);
    
    // Step 3: Statement → Plan（优化器）
    std::unique_ptr<query::ExecutionPlan> optimize(query::Statement* stmt);
    
    // Step 4: Plan → Executor（执行器）
    ExecResult execute_plan(query::ExecutionPlan* plan);
    
    // ============================================================
    // 具体语句执行
    // ============================================================
    ExecResult execute_select(query::SelectStatement* stmt);
    ExecResult execute_insert(query::InsertStatement* stmt);
    ExecResult execute_update(query::UpdateStatement* stmt);
    ExecResult execute_delete(query::DeleteStatement* stmt);
    ExecResult execute_use(query::UseStatement* stmt);
    ExecResult execute_ddl(query::Statement* stmt);
    
    // ============================================================
    // 辅助方法
    // ============================================================
    sql::TableSchema get_table_schema(const std::string& table_name);
    std::shared_ptr<sql::Table> get_table(const std::string& table_name);
    std::shared_ptr<sql::Database> get_database();
    void reset_parser_state();

    // ============================================================
    // 成员变量
    // ============================================================
    std::shared_ptr<sql::DatabaseManager> db_manager_;
    std::string current_db_;
    
    // 中端组件
    std::unique_ptr<query::StatementBuilder> builder_;
    std::unique_ptr<query::Optimizer> optimizer_;
    std::unique_ptr<query::Executor> executor_;
    
    // 统计
    size_t query_count_{0};
    size_t update_count_{0};
};

// ============================================================
// 表结构构建器（用于 CREATE TABLE）
// ============================================================
class TableSchemaBuilder {
public:
    TableSchemaBuilder(const std::string& name);
    
    TableSchemaBuilder& column(const std::string& name, sql::DataType type, 
                               bool nullable = true, bool pk = false);
    
    TableSchemaBuilder& primary_key(const std::string& name, sql::DataType type);
    
    TableSchemaBuilder& not_null(const std::string& name, sql::DataType type);
    
    TableSchemaBuilder& nullable(const std::string& name, sql::DataType type);
    
    sql::TableSchema build() const { return schema_; }

private:
    sql::TableSchema schema_;
};

} // namespace session

#endif // SESSION_H