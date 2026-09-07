#ifndef PARSER_PARSER_H
#define PARSER_PARSER_H


#include <memory>
#include <string>
#include <optional>
#include <vector>

#include "ast.h"

#ifdef  __cplusplus
extern "C" {
#endif 

void yyerror(const char *s);

#ifdef __cplusplus
}
#endif

namespace parser {

// ============================================================
// ASTNode 智能指针（unique_ptr + 自定义删除器）
// ============================================================
struct ASTNodeDeleter {
    void operator()(ASTNode* node) const {
        if (node != nullptr) { free_ast(node);
}
    }
};

using ASTNodePtr = std::unique_ptr<ASTNode, ASTNodeDeleter>;

// ============================================================
// 解析错误
// ============================================================
struct ParseError {
    int line = 0;
    int column = 0;
    std::string message;
    std::string token;
    
    ParseError() = default;
    explicit ParseError(const std::string& msg) : message(msg) {}
    ParseError(int l, int c, const std::string& msg, const std::string& tok = "")
        : line(l), column(c), message(msg), token(tok) {}
    
    std::string to_string() const {
        if (line > 0) {
            return "Parse error at line " + std::to_string(line) + 
                   ", column " + std::to_string(column) + ": " + message;
        }
        return "Parse error: " + message;
    }
};

// ============================================================
// 解析结果
// ============================================================
struct ParseResult {
    std::string sql;
    bool success;
    ASTNodePtr ast;
    std::optional<ParseError> error;
    
    ParseResult() : success(false) {}
    explicit ParseResult(const std::string& stmt, ASTNode* node) : sql(stmt), success(true), ast(node) {}
    explicit ParseResult(const std::string& stmt, const ParseError& err) : sql(stmt), success(false), error(err) {}
    
    // 移动语义
    ParseResult(ParseResult&& other) noexcept
        : sql(other.sql), success(other.success), ast(std::move(other.ast)), error(std::move(other.error)) {}
    
    ParseResult& operator=(ParseResult&& other) noexcept {
        if (this != &other) {
            sql = std::move(other.sql);
            success = other.success;
            ast = std::move(other.ast);
            error = std::move(other.error);
        }
        return *this;
    }
    
    // 禁止拷贝
    ParseResult(const ParseResult&) = delete;
    ParseResult& operator=(const ParseResult&) = delete;
    
    // 访问
    const char * c_sql() const { return sql.c_str(); }
    const std::string& s_sql() const { return sql; }
    const ASTNode* get() const { return ast.get(); }
    ASTNode* get() { return ast.get(); }
    ASTNode* operator->() { return ast.get(); }
    const ASTNode* operator->() const { return ast.get(); }
    ASTNode& operator*() { return *ast; }
    const ASTNode& operator*() const { return *ast; }
    
    explicit operator bool() const { return success; }
    
    // 释放所有权（谨慎使用）
    ASTNode* release() { return ast.release(); }
};

// ============================================================
// Parser 类
// ============================================================
class Parser {
public:
    Parser();
    ~Parser();
    
    // 解析单条 SQL 语句
    ParseResult parse(const std::string& sql);
    
    // 解析多条 SQL 语句（用分号分隔）
    std::vector<ParseResult> parse_multi(const std::string& sql);
    
    // 设置调试模式
    void set_debug(bool enable) { debug_ = enable; }
    bool debug_enabled() const { return debug_; }
    
    // 获取最后错误信息
    std::string last_error() const { return last_error_; }
    void set_last_error(const std::string& err) { last_error_ = err; }
    
    // 获取统计信息
    size_t parse_count() const { return parse_count_; }
    size_t error_count() const { return error_count_; }
    
    // 重置状态
    void reset();

private:
    bool do_parse(const std::string& sql, ASTNode** result);
    std::string error_detail() const;
    
    std::string last_error_;
    bool debug_ = false;
    size_t parse_count_ = 0;
    size_t error_count_ = 0;
};

// ============================================================
// 便捷函数
// ============================================================
// 解析 SQL，返回裸指针（调用者负责释放）
ASTNode* parse_sql(const std::string& sql, std::string* error = nullptr);

// 解析 SQL，返回智能指针（自动管理）
ASTNodePtr parse_sql_smart(const std::string& sql, std::string* error = nullptr);

} // namespace parser


#endif // PARSER_PARSER_H