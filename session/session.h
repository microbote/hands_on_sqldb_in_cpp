// session.h
//
// Session：把一个 SQL 文本串成完整链路，交给客户端一个游标。
//
//   execute(sql)
//       |
//       |  Parser -> StatementBuilder -> StatementValidator -> QueryRewriter
//       |         -> Optimizer -> Planner -> ExecutorFactory
//       v
//   std::unique_ptr<exec::ResultCursor>（实现 sql::Cursor）
//
// 职责边界：
//   - Session 持有**连接级状态**：一个 KV 引擎 + KVCatalog（含"当前数据库"），
//     以及这条流水线的每一段（都是无状态对象，可以复用）；
//   - DDL / USE 没有行流，不进计划器：这里直接落到 Catalog（建/删库表、切库），
//     然后返回一个"空游标"，让"一条 SQL -> 一个游标"的约定保持统一；
//   - DML：校验 -> 重写 -> 优化 -> 计划 -> 执行；写语句在 execute() 里就跑完，
//     受影响行数从 `cursor->affected_rows()` 读；
//   - 错误一律作为值返回（SessionError：错误码 + 信息 + 出错位置 + 高亮片段），
//     库代码不打印 —— CLI 想怎么显示（含颜色）由调用方决定。
#pragma once

#include <cstdint>
#include <expected>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "common/source_span.h"
#include "executor/executor.h"
#include "parser/parser.h"
#include "relation/kv_catalog.h"
#include "sql_types/cursor.h"
#include "sql_types/identifier.h"

namespace session {

// ============================================================
// 元信息（元命令 \l / \dt / \d 用）
// ============================================================
struct DatabaseInfo {
  sql::Identifier name;
  int64_t created_at = 0; // Unix 秒（0 = 老数据没有统计记录）
  size_t table_count = 0;
  bool is_current = false;
};

struct TableInfo {
  sql::Identifier name;
  size_t column_count = 0;
  sql::Identifier primary_key; // 空 = 无主键
  int64_t created_at = 0;
  int64_t last_write_at = 0; // 0 = 还没有写语句改过它
  size_t row_count = 0;      // 现算的（全表扫描，O(n)）
};

// ============================================================
// 错误
// ============================================================
enum class SessionErrorCode : uint8_t {
  OK = 0,
  EMPTY_SQL,            // 空语句
  PARSE_ERROR,          // 词法/语法
  BUILD_ERROR,          // AST -> Query
  VALIDATE_ERROR,       // 语义校验（库/表/列不存在、类型不匹配……）
  REWRITE_ERROR,        // 查询重写
  OPTIMIZE_ERROR,       // 优化（主键抽取等）
  PLAN_ERROR,           // 生成计划
  EXECUTE_ERROR,        // 执行（含 DDL 落库失败）
  CONSTRAINT_VIOLATION, // 约束不满足（主键重复等）
  NOT_SUPPORTED,        // 该语句类型这里不处理
  TRANSACTION_ERROR, // 事务控制错误（重复 BEGIN / COMMIT 无事务 / 事务已中止）
};

inline const char *session_error_message(SessionErrorCode code) {
  switch (code) {
  case SessionErrorCode::OK:
    return "OK";
  case SessionErrorCode::EMPTY_SQL:
    return "Empty statement";
  case SessionErrorCode::PARSE_ERROR:
    return "Syntax error";
  case SessionErrorCode::BUILD_ERROR:
    return "Statement build error";
  case SessionErrorCode::VALIDATE_ERROR:
    return "Statement validation error";
  case SessionErrorCode::REWRITE_ERROR:
    return "Query rewrite error";
  case SessionErrorCode::OPTIMIZE_ERROR:
    return "Query optimize error";
  case SessionErrorCode::PLAN_ERROR:
    return "Plan error";
  case SessionErrorCode::EXECUTE_ERROR:
    return "Execution error";
  case SessionErrorCode::CONSTRAINT_VIOLATION:
    return "Constraint violation";
  case SessionErrorCode::NOT_SUPPORTED:
    return "Statement is not supported";
  case SessionErrorCode::TRANSACTION_ERROR:
    return "Transaction error";
  default:
    return "Unknown error";
  }
}

struct SessionError {
  SessionErrorCode code = SessionErrorCode::OK;
  std::string message;          // 带上下文的可读信息（含表名/列名等）
  std::string sql;              // 原始语句（回显/日志用）
  SSpan span = sspan_unknown(); // 出错位置（可能未知）

  SessionError() = default;
  SessionError(SessionErrorCode c, std::string msg, std::string statement)
      : code(c), message(std::move(msg)), sql(std::move(statement)) {}
  SessionError(SessionErrorCode c, std::string msg, std::string statement,
               SSpan where)
      : code(c), message(std::move(msg)), sql(std::move(statement)),
        span(where) {}

  bool ok() const { return code == SessionErrorCode::OK; }
  explicit operator bool() const { return ok(); }

  // "信息 (line L:C)"；位置未知时只返回信息
  std::string to_string() const {
    const std::string base =
        message.empty() ? std::string(session_error_message(code)) : message;
    if (!sspan_valid(span)) {
      return base;
    }
    return base + " (line " + std::to_string(span.begin_line) + ":" +
           std::to_string(span.begin_column) + ")";
  }

  // 出错片段着色（由 stmt::highlight_span 实现，见
  // statement/sql_highlight.h）； 位置未知时返回空串
  std::string highlight(bool colors = true) const;
};

// ============================================================
// 解析产物（parse 与 execute 两段的接口）
//
//   parsed = session.parse(sql)          // 纯解析：可以在**别的线程**（parser
//                                        // 服务线程）上做，产物 move 回来
//   cursor = session.execute_parsed(parsed)   // 建 Query + 校验 + 执行
//
// 拆两段是为了服务器：解析有进程级全局状态，只允许一个线程做；建 Query /
// 校验 / 执行没有全局状态，留在会话自己的线程上。本地调用者继续用
// `execute(sql)`（内部就是这两步）。
// ============================================================
struct ParsedStatement {
  parser::ASTNodePtr ast; // 整棵 AST（含 EXPLAIN 前缀节点，如果有）
  bool explain = false;   // 带 EXPLAIN 前缀
  bool analyze = false;   // EXPLAIN ANALYZE
  // 只读语句（SELECT，或 EXPLAIN ... SELECT）：服务器据此路由 ——
  // 只读且不在事务里 -> 读线程就地执行；其余 -> 写线程排队。
  // 由 AST 根节点类型推出（不看 SQL 文本，避免字符串启发式）。
  bool read_only = false;
  std::string sql; // 原文（诊断用）

  ParsedStatement() = default;
  ParsedStatement(parser::ASTNodePtr tree, bool with_explain, bool with_analyze,
                  bool is_read_only, std::string text)
      : ast(std::move(tree)), explain(with_explain), analyze(with_analyze),
        read_only(is_read_only), sql(std::move(text)) {}
};

// ============================================================
// Session
// ============================================================
class Session {
public:
  // engine 必须已经 open；Session 用它建自己的 Catalog（连接级视图）
  // clock 可不传（默认系统时间）；测试里注入假时钟可做确定性断言
  explicit Session(std::shared_ptr<kv::KVEngine> engine,
                   sql::KVCatalog::Clock clock = {});
  ~Session() = default;

  Session(const Session &) = delete;
  Session &operator=(const Session &) = delete;

  // 核心入口：一条 SQL -> 结果游标
  //
  //   SELECT 类：惰性执行，第一次 next() 才真正开始扫；
  //   写语句：这里就执行完（affected_rows() 读受影响行数）；
  //   DDL/USE：落到 Catalog，返回空游标；
  //   EXPLAIN [ANALYZE] <语句>：返回**单列结果集**（列名 "QUERY PLAN"，
  //     每个元素一行），和别的语句一样走 next()。
  //
  // EXPLAIN 由语法层识别（parser/sql.y 的 explain_stmt），session 把它拆掉：
  // 被解释的语句照常走 builder/validator/optimizer/planner，只是不执行
  // （ANALYZE 时才真的执行一遍并带上统计）。DDL/USE 没有计划树 ->
  // NOT_SUPPORTED；ANALYZE 只允许 SELECT（写语句会真的改数据）。
  std::expected<std::unique_ptr<exec::ResultCursor>, SessionError>
  execute(const std::string &sql);

  // 只解析（不建 Query、不校验）：服务器把它丢到 parser 服务线程上跑
  std::expected<ParsedStatement, SessionError> parse(const std::string &sql);
  // 执行一个已经解析好的语句（建 Query + 校验 + 执行）
  std::expected<std::unique_ptr<exec::ResultCursor>, SessionError>
  execute_parsed(const ParsedStatement &parsed);
  // 解析失败的统一收尾：中止的事务里优先报"事务已中止"
  // （服务器把 parse 放到别的线程上跑，所以这个收尾要能单独调用）
  SessionError
  parse_error_to_session_error(const SessionError &parse_error) const;

  // 显式事务：BEGIN / COMMIT / ROLLBACK（语法层认出来的语句，
  // 见 parser/sql.y 的 transaction_stmt；执行与会话状态在这里）。
  //   - BEGIN 取**快照**（只读事务的可重复读），不抢写槽；
  //   - 第一条**写**语句才抢写槽（单写者），抢不到报 busy；
  //   - 在事务里时，语句的自动提交让位给事务本身（由 COMMIT/ROLLBACK 收尾）。
  bool in_transaction() const { return in_transaction_; }

  // ---- 元信息查询（元命令用；db 为空表示当前数据库）----
  std::vector<DatabaseInfo> databases() const;
  std::vector<TableInfo> tables(const sql::Identifier &db = {}) const;
  // 表的 schema；表不存在返回 nullopt
  std::optional<sql::TableSchema>
  table_schema(const sql::Identifier &table,
               const sql::Identifier &db = {}) const;

  // 连接级状态
  const sql::KVCatalog &catalog() const { return catalog_; }
  sql::KVCatalog &catalog() { return catalog_; }
  sql::Identifier current_database() const {
    return catalog_.current_database();
  }

private:
  // pipeline 前半段：解析 + 建 Query + 语义校验（execute/explain 共用）
  //
  // EXPLAIN 前缀只影响"这条语句要不要执行"（以及要不要带统计），
  // 所以在这里就拆掉：query 始终是**被解释的那条语句**。
  struct Prepared {
    sql::Query query;
    bool explain = false; // EXPLAIN 前缀
    bool analyze = false; // EXPLAIN ANALYZE
  };
  std::expected<Prepared, SessionError> prepare(const ParsedStatement &parsed);

  // pipeline 后半段：重写 -> 优化 -> 生成计划树（execute/explain 共用）
  std::expected<std::unique_ptr<plan::PlanNode>, SessionError>
  build_plan(const sql::Query &query, const std::string &statement);

  // EXPLAIN：生成计划（ANALYZE 时执行一遍拿统计），把结果变成
  // 单列文本结果集
  std::expected<std::unique_ptr<exec::ResultCursor>, SessionError>
  execute_explain(const sql::Query &query, bool analyze,
                  const std::string &statement);

  // 把 KVCatalog 的统计（维护着的行数）喂给优化器的成本模型
  plan::StatsProvider stats_provider() const;

  // 事务控制
  std::expected<std::unique_ptr<exec::ResultCursor>, SessionError>
  begin_transaction(const std::string &statement);
  std::expected<std::unique_ptr<exec::ResultCursor>, SessionError>
  commit_transaction(const std::string &statement);
  std::expected<std::unique_ptr<exec::ResultCursor>, SessionError>
  rollback_transaction(const std::string &statement);

  // DDL / USE：不需要计划器，直接落到 Catalog
  std::expected<std::unique_ptr<exec::ResultCursor>, SessionError>
  execute_catalog_statement(const sql::Query &query, const std::string &sql);

  std::shared_ptr<kv::KVEngine> engine_;
  sql::KVCatalog catalog_;
  bool in_transaction_ = false; // 显式事务是否打开
  bool tx_failed_ = false;      // 事务里出过错：只能 ROLLBACK（Postgres 风格）
};

} // namespace session
