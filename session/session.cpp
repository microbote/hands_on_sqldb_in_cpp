// session.cpp
#include "session.h"

#include <cctype>
#include <string>
#include <utility>

#include "parser/parser.h"
#include "planner/optimizer.h"
#include "planner/planner.h"
#include "planner/rewriter.h"
#include "sql_types/query.h"
#include "sql_types/row.h"
#include "sql_types/schema.h"
#include "statement/sql_highlight.h"
#include "statement/stmt_builder.h"
#include "statement/stmt_validator.h"
#include "storage/kv_engine/kv_engine.h"

namespace session {
namespace {

// 去掉尾部空白；没有结尾分号就补一个（parser 要求语句以 ';' 结束）
std::string normalize_sql(const std::string &sql) {
  size_t end = sql.size();
  while (end > 0 && std::isspace(static_cast<unsigned char>(sql[end - 1]))) {
    --end;
  }
  std::string trimmed = sql.substr(0, end);
  if (!trimmed.empty() && trimmed.back() != ';') {
    trimmed.push_back(';');
  }
  return trimmed;
}

SessionError from_stmt_error(SessionErrorCode code,
                             const stmt::StmtError &error,
                             const std::string &sql) {
  // 只用 StmtError 的 message：位置信息由 SessionError 自己带，
  // 否则 to_string() 会把 "(line L:C)" 打印两遍。
  const std::string message =
      error.message.empty() ? std::string(stmt::stmt_error_message(error.code))
                            : error.message;
  return SessionError(code, message, sql, error.span);
}

SessionError from_plan_error(SessionErrorCode code,
                             const plan::PlanError &error,
                             const std::string &sql) {
  return SessionError(code, error.to_string(), sql);
}

SessionError from_exec_error(const exec::ExecError &error,
                             const std::string &sql) {
  const SessionErrorCode code =
      error.code == exec::ExecErrorCode::CONSTRAINT_VIOLATION
          ? SessionErrorCode::CONSTRAINT_VIOLATION
          : SessionErrorCode::EXECUTE_ERROR;
  return SessionError(code, error.to_string(), sql);
}

SessionError from_cursor_error(const sql::CursorError &error,
                               const std::string &sql) {
  const SessionErrorCode code =
      error.code == sql::CursorErrorCode::CONSTRAINT_VIOLATION
          ? SessionErrorCode::CONSTRAINT_VIOLATION
          : SessionErrorCode::EXECUTE_ERROR;
  return SessionError(code, error.to_string(), sql);
}

// 把多行文本切成行（EXPLAIN 的"一行一个结果行"用）：
// 末尾的空行不产生结果行（否则会多出一行空行）。
std::vector<std::string> split_lines(const std::string &text) {
  std::vector<std::string> lines;
  size_t begin = 0;
  while (begin < text.size()) {
    const size_t end = text.find('\n', begin);
    if (end == std::string::npos) {
      lines.push_back(text.substr(begin));
      break;
    }
    lines.push_back(text.substr(begin, end - begin));
    begin = end + 1;
  }
  return lines;
}

// 计划里目标表的库名/表名（扫描节点或写节点上都带着）
sql::Identifier plan_db(const plan::PlanNode &root) {
  for (const plan::PlanNode *node = &root; node != nullptr;
       node = node->child()) {
    if (const auto *query_node =
            dynamic_cast<const plan::QueryPlanNode *>(node)) {
      if (!query_node->query().db.empty()) {
        return query_node->query().db;
      }
    }
    if (const auto *scan = dynamic_cast<const plan::ScanPlan *>(node)) {
      if (!scan->target().db.empty()) {
        return scan->target().db;
      }
    }
  }
  return sql::Identifier();
}

sql::Identifier plan_table(const plan::PlanNode &root) {
  for (const plan::PlanNode *node = &root; node != nullptr;
       node = node->child()) {
    if (const auto *query_node =
            dynamic_cast<const plan::QueryPlanNode *>(node)) {
      if (!query_node->query().table.empty()) {
        return query_node->query().table;
      }
    }
    if (const auto *scan = dynamic_cast<const plan::ScanPlan *>(node)) {
      if (!scan->target().table.empty()) {
        return scan->target().table;
      }
    }
  }
  return sql::Identifier();
}

// CREATE TABLE 的列定义直接来自语句（validator 已经校验过合法性）
sql::TableSchema schema_from_query(const sql::CreateTableQuery &query) {
  sql::TableSchema schema(query.table);
  for (const auto &column : query.columns) {
    schema.add_column(column);
  }
  return schema;
}

// ============================================================
// 自动提交守卫：整个语句一个事务
//
//   - 构造时 begin（引擎已经有事务时——将来的显式 BEGIN——就加入它，不重复开）；
//   - commit() 显式提交并解除守卫；
//   - 析构时若还没提交 -> 自动 rollback：**任何提前 return 都不会留下半截写**。
//     这就是"语句原子性"的实现方式。
// ============================================================
class AutoCommit {
public:
  explicit AutoCommit(kv::KVEngine *engine) : engine_(engine) {
    if (engine_ != nullptr && !engine_->in_transaction()) {
      active_ = engine_->begin_transaction() == kv::Status::OK;
    }
  }
  ~AutoCommit() {
    if (active_ && engine_ != nullptr) {
      engine_->rollback_transaction();
    }
  }
  AutoCommit(const AutoCommit &) = delete;
  AutoCommit &operator=(const AutoCommit &) = delete;

  bool active() const { return active_; }

  kv::Status commit() {
    if (!active_ || engine_ == nullptr) {
      return kv::Status::OK;
    }
    const kv::Status status = engine_->commit_transaction();
    if (status == kv::Status::OK) {
      active_ = false; // 提交成功：解除守卫，析构时不再回滚
    }
    return status;
  }

private:
  kv::KVEngine *engine_;
  bool active_ = false;
};

} // namespace

std::string SessionError::highlight(bool colors) const {
  if (!sspan_valid(span) || sql.empty()) {
    return std::string();
  }
  return stmt::highlight_span(sql, span, colors);
}

// ============================================================
// Session
// ============================================================
Session::Session(std::shared_ptr<kv::KVEngine> engine,
                 sql::KVCatalog::Clock clock)
    : engine_(std::move(engine)), catalog_(engine_, std::move(clock)) {}

// 优化器的成本模型只需要一个数：表行数（拿不到就 rows_known = false，
// 这时优化器不做基于成本的决策，退回纯规则行为）
plan::StatsProvider Session::stats_provider() const {
  return [this](const sql::Identifier &db, const sql::Identifier &table) {
    plan::RelationStats stats;
    auto table_stats = catalog_.table_stats(db, table);
    if (table_stats.has_value() && table_stats->row_count >= 0) {
      stats.rows = table_stats->row_count;
      stats.rows_known = true;
    }
    return stats;
  };
}

// pipeline 前半段：解析 + 建 Query + 语义校验（execute/explain 共用）
std::expected<Session::Prepared, SessionError>
Session::prepare(const std::string &statement) {
  // ---- 1) 解析 ----
  parser::Parser parser;
  auto parsed = parser.parse(statement);
  if (!parsed.success || parsed.ast == nullptr) {
    SessionError error(SessionErrorCode::PARSE_ERROR,
                       parsed.error.has_value() ? parsed.error->message
                                                : "parse failed",
                       statement);
    if (parsed.error.has_value()) {
      const size_t length =
          parsed.error->token.empty() ? 1 : parsed.error->token.size();
      error.span =
          sspan_make(static_cast<uint32_t>(parsed.error->line),
                     static_cast<uint32_t>(parsed.error->column),
                     static_cast<uint32_t>(parsed.error->line),
                     static_cast<uint32_t>(parsed.error->column + length));
    }
    return std::unexpected(std::move(error));
  }

  // ---- 2) 拆掉 EXPLAIN 前缀（语法层给的 NODE_EXPLAIN）----
  // 被解释的还是原来那条语句：位置、错误的处理都跟平时一样
  const ASTNode *root = parsed.ast.get();
  bool explain = false;
  bool analyze = false;
  if (root->type == NODE_EXPLAIN) {
    const auto *node = reinterpret_cast<const ExplainNode *>(root->data);
    explain = true;
    analyze = node->analyze != 0;
    root = node->statement;
    if (root == nullptr) {
      return std::unexpected(SessionError(SessionErrorCode::BUILD_ERROR,
                                          "EXPLAIN has no statement",
                                          statement));
    }
  }

  // ---- 3) AST -> Query ----
  stmt::StatementBuilder builder;
  auto query = builder.build(root);
  if (!query.has_value()) {
    return std::unexpected(from_stmt_error(SessionErrorCode::BUILD_ERROR,
                                           query.error(), statement));
  }

  // ---- 4) 语义校验（带"名字 -> 位置"解析器，错误能定位到 SQL 片段）----
  stmt::StatementValidator validator(catalog_, stmt::make_span_resolver(root));
  auto valid = validator.validate(*query);
  if (!valid.has_value()) {
    return std::unexpected(from_stmt_error(SessionErrorCode::VALIDATE_ERROR,
                                           valid.error(), statement));
  }
  return Prepared{std::move(*query), explain, analyze};
}

// pipeline 后半段：重写 -> 优化 -> 生成计划树（execute/explain 共用）
std::expected<std::unique_ptr<plan::PlanNode>, SessionError>
Session::build_plan(const sql::Query &query, const std::string &statement) {
  // 查询重写
  plan::QueryRewriter rewriter;
  auto rewritten = rewriter.rewrite(query);
  if (!rewritten.has_value()) {
    return std::unexpected(from_plan_error(SessionErrorCode::REWRITE_ERROR,
                                           rewritten.error(), statement));
  }

  // 优化：主键条件 -> 有序区间 + 残余过滤
  plan::Optimizer optimizer(catalog_, stats_provider());
  auto optimized = optimizer.optimize(*rewritten);
  if (!optimized.has_value()) {
    return std::unexpected(from_plan_error(SessionErrorCode::OPTIMIZE_ERROR,
                                           optimized.error(), statement));
  }

  // 生成计划树
  plan::Planner planner;
  auto planned = planner.plan(std::move(*optimized));
  if (!planned.has_value()) {
    return std::unexpected(from_plan_error(SessionErrorCode::PLAN_ERROR,
                                           planned.error(), statement));
  }
  return std::move(*planned);
}

std::expected<std::unique_ptr<exec::ResultCursor>, SessionError>
Session::execute(const std::string &sql) {
  const std::string statement = normalize_sql(sql);
  if (statement.empty() || statement == ";") {
    return std::unexpected(
        SessionError(SessionErrorCode::EMPTY_SQL, "empty statement", sql));
  }
  if (!catalog_.is_open()) {
    return std::unexpected(SessionError(SessionErrorCode::EXECUTE_ERROR,
                                        "storage engine is not open",
                                        statement));
  }

  auto prepared = prepare(statement);
  // ---- 事务控制：BEGIN / COMMIT / ROLLBACK（语法层认出来，这里执行）----
  // 放在"事务已中止"检查之前：中止状态下唯一还能跑的就是 ROLLBACK。
  // `EXPLAIN BEGIN` 不走这条（下面单独处理）：被解释的事务语句不会执行。
  if (prepared.has_value() && !prepared->explain) {
    if (const sql::TransactionStmt *tx = prepared->query.transaction();
        tx != nullptr) {
      switch (tx->kind) {
      case sql::TransactionKind::BEGIN:
        return begin_transaction(statement);
      case sql::TransactionKind::COMMIT:
        return commit_transaction(statement);
      case sql::TransactionKind::ROLLBACK:
        return rollback_transaction(statement);
      }
    }
  }
  // 事务里出过错之后只允许 ROLLBACK：否则失败的语句会留下半截改动
  // （这里先报"事务已中止"，连语法错误也被它盖住 —— 与 Postgres 一致）
  if (tx_failed_) {
    return std::unexpected(SessionError(
        SessionErrorCode::TRANSACTION_ERROR,
        "current transaction is aborted; issue ROLLBACK", statement));
  }
  if (!prepared.has_value()) {
    return std::unexpected(prepared.error());
  }
  const sql::Query &query = prepared->query;

  // EXPLAIN：被解释的语句**不执行**（ANALYZE 才真的跑一遍），输出是
  // 单列文本结果集。放在"事务已中止"检查之后 —— 中止的事务里连 EXPLAIN
  // 也不许跑（ANALYZE 会真的读数据），和 Postgres 的语义一致。
  if (prepared->explain) {
    return execute_explain(query, prepared->analyze, statement);
  }

  // 写语句 / DDL / USE：整个语句包成一个事务（自动提交）。
  // 读语句不开事务（不需要原子性，也不该长期占着写锁）。
  // 守卫在析构时自动回滚 —— 下面任何提前 return 都不会留下半截写。
  const bool own_transaction = !in_transaction_; // 显式事务里让位给事务本身
  AutoCommit auto_commit((own_transaction && !query.is_select()) ? engine_.get()
                                                                 : nullptr);
  if (own_transaction && !query.is_select() && !auto_commit.active()) {
    return std::unexpected(SessionError(SessionErrorCode::EXECUTE_ERROR,
                                        "cannot begin transaction (busy?)",
                                        statement));
  }
  const auto finish =
      [&](std::expected<std::unique_ptr<exec::ResultCursor>, SessionError>
              result) {
        if (!result.has_value()) {
          if (in_transaction_) {
            tx_failed_ = true; // 事务里出错：只能 ROLLBACK（Postgres 风格）
          }
          return result; // 守卫析构自动回滚
        }
        const kv::Status committed = auto_commit.commit();
        if (committed != kv::Status::OK) {
          return std::expected<std::unique_ptr<exec::ResultCursor>,
                               SessionError>(std::unexpected(SessionError(
              SessionErrorCode::EXECUTE_ERROR,
              std::string("commit failed: ") + kv::status_to_string(committed),
              statement)));
        }
        return result;
      };

  // DDL / USE：没有行流，直接落到 Catalog
  if (query.is_ddl() || query.is_ctrl()) {
    return finish(execute_catalog_statement(query, statement));
  }
  if (!query.is_dml()) {
    return std::unexpected(
        SessionError(SessionErrorCode::NOT_SUPPORTED,
                     "unsupported statement: " + query.to_string(), statement));
  }

  auto planned = build_plan(query, statement);
  if (!planned.has_value()) {
    return std::unexpected(planned.error());
  }

  // 打开表（执行器按表视图落 KV 操作）
  auto table = catalog_.open_table(plan_db(**planned), plan_table(**planned));
  if (!table.has_value()) {
    return finish(
        std::unexpected(SessionError(SessionErrorCode::EXECUTE_ERROR,
                                     table.error().to_string(), statement)));
  }
  auto shared_table = std::make_shared<sql::Table>(std::move(*table));

  // 执行（写语句在这里就跑完；SELECT 惰性，第一次 next() 才启动）
  auto cursor = exec::execute(**planned, std::move(shared_table));
  if (!cursor.has_value()) {
    return finish(std::unexpected(from_exec_error(cursor.error(), statement)));
  }
  // 写语句：立即检查是否出错（错误不会等到 next() 才暴露）
  if (!(*cursor)->root()->produces_rows() && (*cursor)->error().is_error()) {
    return finish(
        std::unexpected(from_cursor_error((*cursor)->error(), statement)));
  }
  // 写语句真的改了行 -> 更新这张表的"最后写入时间"与行数（成本模型/元命令用）
  if ((*cursor)->root()->is_write() && (*cursor)->affected_rows() > 0) {
    const int64_t affected = static_cast<int64_t>((*cursor)->affected_rows());
    int64_t delta = 0; // UPDATE 不改行数
    if (query.is_insert()) {
      delta = affected;
    } else if (query.is_delete()) {
      delta = -affected;
    }
    catalog_.touch_table(plan_db(**planned), plan_table(**planned), delta);
  }
  return finish(std::move(*cursor));
}

// ============================================================
// 事务控制（BEGIN / COMMIT / ROLLBACK）
// ============================================================
std::expected<std::unique_ptr<exec::ResultCursor>, SessionError>
Session::begin_transaction(const std::string &statement) {
  if (in_transaction_) {
    return std::unexpected(SessionError(SessionErrorCode::TRANSACTION_ERROR,
                                        "already in a transaction", statement));
  }
  if (engine_ == nullptr) {
    return std::unexpected(SessionError(SessionErrorCode::EXECUTE_ERROR,
                                        "no storage engine", statement));
  }
  const kv::Status status = engine_->begin_transaction();
  if (status != kv::Status::OK) {
    return std::unexpected(
        SessionError(SessionErrorCode::TRANSACTION_ERROR,
                     std::string("cannot begin transaction: ") +
                         kv::status_to_string(status),
                     statement));
  }
  in_transaction_ = true;
  tx_failed_ = false;
  return exec::empty_result();
}

std::expected<std::unique_ptr<exec::ResultCursor>, SessionError>
Session::commit_transaction(const std::string &statement) {
  if (!in_transaction_ || engine_ == nullptr) {
    return std::unexpected(SessionError(SessionErrorCode::TRANSACTION_ERROR,
                                        "no transaction to commit", statement));
  }
  if (tx_failed_) {
    // 事务已经坏了：COMMIT 实际执行回滚，并明确告诉调用方
    engine_->rollback_transaction();
    in_transaction_ = false;
    tx_failed_ = false;
    return std::unexpected(SessionError(
        SessionErrorCode::TRANSACTION_ERROR,
        "transaction was aborted; rolled back instead of committing",
        statement));
  }
  const kv::Status status = engine_->commit_transaction();
  if (status != kv::Status::OK) {
    // 缓冲还留在引擎里：可以重试 COMMIT，也可以 ROLLBACK
    return std::unexpected(SessionError(SessionErrorCode::TRANSACTION_ERROR,
                                        std::string("commit failed: ") +
                                            kv::status_to_string(status),
                                        statement));
  }
  in_transaction_ = false;
  return exec::empty_result();
}

std::expected<std::unique_ptr<exec::ResultCursor>, SessionError>
Session::rollback_transaction(const std::string &statement) {
  if (!in_transaction_ || engine_ == nullptr) {
    return std::unexpected(SessionError(SessionErrorCode::TRANSACTION_ERROR,
                                        "no transaction to roll back",
                                        statement));
  }
  const kv::Status status = engine_->rollback_transaction();
  in_transaction_ = false;
  tx_failed_ = false;
  if (status != kv::Status::OK) {
    return std::unexpected(SessionError(SessionErrorCode::TRANSACTION_ERROR,
                                        std::string("rollback failed: ") +
                                            kv::status_to_string(status),
                                        statement));
  }
  return exec::empty_result();
}

// ============================================================
// 元信息查询（元命令用）
// ============================================================
std::vector<DatabaseInfo> Session::databases() const {
  std::vector<DatabaseInfo> result;
  if (!catalog_.is_open()) {
    return result;
  }
  const sql::Identifier current = catalog_.current_database();
  for (const sql::Identifier &db : catalog_.list_databases()) {
    DatabaseInfo info;
    info.name = db;
    info.is_current = !current.empty() && db == current;
    info.table_count = catalog_.list_tables(db).size();
    auto stats = catalog_.database_stats(db);
    if (stats.has_value()) {
      info.created_at = stats->created_at;
    }
    result.push_back(std::move(info));
  }
  return result;
}

std::vector<TableInfo> Session::tables(const sql::Identifier &db) const {
  std::vector<TableInfo> result;
  if (!catalog_.is_open()) {
    return result;
  }
  const sql::Identifier target = db.empty() ? catalog_.current_database() : db;
  if (target.empty() || !catalog_.database_exists(target)) {
    return result;
  }
  for (const sql::Identifier &table : catalog_.list_tables(target)) {
    TableInfo info;
    info.name = table;
    auto schema = catalog_.get_table_schema(target, table);
    if (schema.has_value()) {
      info.column_count = schema->column_count();
      if (schema->has_primary_key()) {
        info.primary_key = schema->primary_key_name();
      }
    }
    auto stats = catalog_.table_stats(target, table);
    if (stats.has_value()) {
      info.created_at = stats->created_at;
      info.last_write_at = stats->last_write_at;
    }
    // 行数现算：不维护计数器（避免写放大与计数漂移），代价 O(n)
    auto opened = catalog_.open_table(target, table);
    if (opened.has_value()) {
      auto count = opened->row_count();
      if (count.has_value()) {
        info.row_count = *count;
      }
    }
    result.push_back(std::move(info));
  }
  return result;
}

std::optional<sql::TableSchema>
Session::table_schema(const sql::Identifier &table,
                      const sql::Identifier &db) const {
  if (!catalog_.is_open() || table.empty()) {
    return std::nullopt;
  }
  const sql::Identifier target = db.empty() ? catalog_.current_database() : db;
  if (target.empty()) {
    return std::nullopt;
  }
  return catalog_.get_table_schema(target, table);
}

// ============================================================
// EXPLAIN
//
// 和别的语句走同一个入口（execute），只是不执行被解释的语句：
//   EXPLAIN <语句>          -> 计划文本（每个算子一行）
//   EXPLAIN ANALYZE <查询>  -> 真跑一遍，每行带上实际行数/耗时
//
// 输出是**单列结果集**（列名 "QUERY PLAN"），所以客户端用同一套
// next()/close() 取行；CLI 不再需要"这是不是 EXPLAIN"的文本判断。
// ============================================================
std::expected<std::unique_ptr<exec::ResultCursor>, SessionError>
Session::execute_explain(const sql::Query &query, bool analyze,
                         const std::string &statement) {
  if (query.is_transaction()) {
    // 事务控制语句执行的是会话状态（开/提交/回滚），没有计划可看
    return std::unexpected(
        SessionError(SessionErrorCode::NOT_SUPPORTED,
                     "EXPLAIN does not apply to transaction statements: " +
                         query.to_string(),
                     statement));
  }
  if (query.is_ddl() || query.is_ctrl() || !query.is_dml()) {
    // DDL/USE 没有计划树：只是 Catalog 上的一次动作
    return std::unexpected(SessionError(
        SessionErrorCode::NOT_SUPPORTED,
        "EXPLAIN only supports SELECT/INSERT/UPDATE/DELETE", statement));
  }

  auto planned = build_plan(query, statement);
  if (!planned.has_value()) {
    return std::unexpected(planned.error());
  }

  // 普通 EXPLAIN：**不执行**，只看计划
  if (!analyze) {
    return exec::text_result("QUERY PLAN",
                             split_lines(plan::plan_tree_to_string(**planned)));
  }

  // EXPLAIN ANALYZE：真的执行一遍。只允许 SELECT —— 写语句会真的改数据，
  // 这里直接拒绝（要分析写语句得把它包在事务里跑完再回滚）。
  if (!query.is_select()) {
    return std::unexpected(SessionError(
        SessionErrorCode::NOT_SUPPORTED,
        "EXPLAIN ANALYZE only supports SELECT (it would really modify data)",
        statement));
  }
  auto table = catalog_.open_table(plan_db(**planned), plan_table(**planned));
  if (!table.has_value()) {
    return std::unexpected(SessionError(SessionErrorCode::EXECUTE_ERROR,
                                        table.error().to_string(), statement));
  }
  auto shared_table = std::make_shared<sql::Table>(std::move(*table));

  exec::ExecReport report;
  auto cursor = exec::execute(**planned, std::move(shared_table), &report);
  if (!cursor.has_value()) {
    return std::unexpected(from_exec_error(cursor.error(), statement));
  }
  // 拉完整个结果流：统计只有跑完才完整
  size_t rows = 0;
  while (true) {
    auto row = (*cursor)->next();
    if (row.has_value()) {
      ++rows;
      continue;
    }
    if (row.error().is_error()) {
      return std::unexpected(from_cursor_error(row.error(), statement));
    }
    break;
  }
  (*cursor)->close();

  std::vector<std::string> lines =
      split_lines(exec::explain_text(**planned, &report));
  lines.push_back("(" + std::to_string(rows) + " row" + (rows == 1 ? "" : "s") +
                  " in result)");
  return exec::text_result("QUERY PLAN", std::move(lines));
}

// ============================================================
// DDL / USE
// ============================================================
std::expected<std::unique_ptr<exec::ResultCursor>, SessionError>
Session::execute_catalog_statement(const sql::Query &query,
                                   const std::string &sql) {
  const auto failed = [&](const std::string &what) {
    return std::unexpected(
        SessionError(SessionErrorCode::EXECUTE_ERROR, what, sql));
  };

  if (const auto *use = query.use_database()) {
    if (!catalog_.use_database(use->database)) {
      return failed("cannot use database: " + use->database.str());
    }
    return exec::empty_result();
  }
  if (const auto *create_db = query.create_database()) {
    if (!catalog_.create_database(create_db->database)) {
      return failed("cannot create database: " + create_db->database.str());
    }
    return exec::empty_result();
  }
  if (const auto *drop_db = query.drop_database()) {
    if (!catalog_.drop_database(drop_db->database)) {
      return failed("cannot drop database: " + drop_db->database.str());
    }
    return exec::empty_result();
  }
  if (const auto *create_table = query.create_table()) {
    const sql::Identifier db = create_table->database.empty()
                                   ? catalog_.current_database()
                                   : create_table->database;
    if (!catalog_.create_table(db, schema_from_query(*create_table))) {
      return failed("cannot create table: " + create_table->table.str());
    }
    return exec::empty_result();
  }
  if (const auto *drop_table = query.drop_table()) {
    const sql::Identifier db = drop_table->database.empty()
                                   ? catalog_.current_database()
                                   : drop_table->database;
    if (!catalog_.drop_table(db, drop_table->table)) {
      return failed("cannot drop table: " + drop_table->table.str());
    }
    return exec::empty_result();
  }
  return failed("unsupported DDL statement: " + query.to_string());
}

} // namespace session
