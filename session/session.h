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
  EMPTY_SQL,      // 空语句
  PARSE_ERROR,    // 词法/语法
  BUILD_ERROR,    // AST -> Query
  VALIDATE_ERROR, // 语义校验（库/表/列不存在、类型不匹配……）
  REWRITE_ERROR,  // 查询重写
  OPTIMIZE_ERROR, // 优化（主键抽取等）
  PLAN_ERROR,     // 生成计划
  EXECUTE_ERROR,  // 执行（含 DDL 落库失败）
  NOT_SUPPORTED,  // 该语句类型这里不处理
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
  case SessionErrorCode::NOT_SUPPORTED:
    return "Statement is not supported";
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
  //   DDL/USE：落到 Catalog，返回空游标。
  std::expected<std::unique_ptr<exec::ResultCursor>, SessionError>
  execute(const std::string &sql);

  // EXPLAIN：只跑 pipeline 到"计划树"为止，**不执行**，返回计划树的文本形式。
  //
  // 入参带不带 EXPLAIN 前缀都行（`EXPLAIN SELECT ...` 与 `SELECT ...` 等价），
  // 方便 CLI 直接把整条语句递进来。DDL/USE 没有计划，返回 NOT_SUPPORTED。
  //
  // analyze = true（`EXPLAIN ANALYZE ...`）：**真的执行一遍**，输出每个算子的
  // 实际行数与耗时。只允许 SELECT —— 写语句会真的改数据，这里直接拒绝。
  std::expected<std::string, SessionError> explain(const std::string &sql,
                                                   bool analyze = false);

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
  std::expected<sql::Query, SessionError> prepare(const std::string &statement);

  // pipeline 后半段：重写 -> 优化 -> 生成计划树（execute/explain 共用）
  std::expected<std::unique_ptr<plan::PlanNode>, SessionError>
  build_plan(const sql::Query &query, const std::string &statement);

  // 把 KVCatalog 的统计（维护着的行数）喂给优化器的成本模型
  plan::StatsProvider stats_provider() const;

  // DDL / USE：不需要计划器，直接落到 Catalog
  std::expected<std::unique_ptr<exec::ResultCursor>, SessionError>
  execute_catalog_statement(const sql::Query &query, const std::string &sql);

  std::shared_ptr<kv::KVEngine> engine_;
  sql::KVCatalog catalog_;
};

} // namespace session
