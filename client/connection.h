// client/connection.h
//
// REPL 只认这一层：**执行语句 + 拿结果**，至于语句是"进程内直接跑"还是
// "发给远程 sqldb-server"由实现决定（LocalConnection / RemoteConnection）。
//
//   - `Outcome` 已经把游标拉完并物化成文本 + NULL 标志：REPL 与 transport
//   无关；
//   - NULL 与空串必须可分（这就是协议里 NULL 标志位的意义）；
//   - 错误带上 `span + 原文`，由客户端渲染 caret（服务端不做 lexer 高亮）。
#pragma once

#include <cstdint>
#include <expected>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "common/source_span.h"
#include "sql_types/schema.h"
#include "storage/kv_engine/kv_engine.h"

namespace client {

// 一个结果单元格
struct Cell {
  bool is_null = false;
  std::string text;
};

// 一条语句的结果
struct Outcome {
  bool ok = false;
  bool has_rows = false; // true = 行流（SELECT/EXPLAIN）；false = 写语句/DDL
  bool is_write = false; // 写语句（affected_rows 有意义）；DDL/USE 为 false
  std::vector<std::string> columns;
  std::vector<std::vector<Cell>> rows;
  uint64_t affected_rows = 0;
  bool in_transaction = false; // 语句执行后会话是否在事务里（提示符用）

  // 失败时
  uint8_t error_code = 0;
  std::string error_message; // 已经带位置（"table not found ... (line 1:15)"）
  std::string error_sql;     // 原文（高亮用）
  SSpan error_span = sspan_unknown();
  // 服务端说"这条语句该去别的节点"（NotLeader + leader hint）：
  // endpoint 非空才能自动重连；只知道 node id 时只能报错。
  uint64_t redirect_node_id = 0;
  std::string redirect_endpoint;
};

// 元命令用的元信息（\l / \dt / \d）
struct DatabaseMeta {
  std::string name;
  int64_t created_at = 0;
  size_t table_count = 0;
  bool is_current = false;
};

struct TableMeta {
  std::string name;
  size_t column_count = 0;
  std::string primary_key;
  int64_t created_at = 0;
  int64_t last_write_at = 0;
  size_t row_count = 0;
};

class SqlConnection {
public:
  virtual ~SqlConnection() = default;

  // 执行一条语句（把游标拉完）
  virtual Outcome execute(const std::string &sql) = 0;
  // 当前库（提示符/`\c` 用；空 = 没选库）
  virtual std::string current_database() const = 0;
  // 语句结束后会话是否在事务里（提示符 * 用）
  virtual bool in_transaction() const = 0;

  // 元信息：远程实现暂不支持（supports_metadata() = false）
  virtual bool supports_metadata() const { return false; }
  virtual std::vector<DatabaseMeta> databases() { return {}; }
  virtual std::vector<TableMeta> tables(const std::string &db) {
    (void)db;
    return {};
  }
  virtual std::optional<sql::TableSchema> table_schema(const std::string &table,
                                                       const std::string &db) {
    (void)table;
    (void)db;
    return std::nullopt;
  }

  // 连接描述（欢迎信息/提示符）
  virtual std::string description() const = 0;

  // 切库：本地与远程都是同一条 SQL（USE 是会话状态）
  bool use_database(const std::string &db) { return execute("USE " + db).ok; }

  // 换到另一个节点执行（服务端回了 NotLeader + leader 地址时）。
  // 成功 = 之后可以在新连接上重发这条语句；默认实现不支持（本地连接、
  // 或服务端没给地址）。
  virtual bool redirect(const std::string &endpoint, std::string *error) {
    (void)endpoint;
    if (error != nullptr) {
      *error = "this connection cannot be redirected";
    }
    return false;
  }
};

// 本地：进程内直接跑（就是现在的 `sqldb`）
std::unique_ptr<SqlConnection> make_local(std::shared_ptr<kv::KVEngine> engine);

// 远程：走自定义协议连 sqldb-server（阻塞式同步客户端）
struct RemoteOptions {
  std::string host = "127.0.0.1";
  std::string port = "5433";
  // 连接级跨组只读模式：loose 允许事务内跨组读。服务端通告能力才发
  // CLIENT_OPTIONS，否则静默保持 strict（默认）。
  bool loose_cross_group_reads = false;
  // 重定向时怎么拿到新连接：默认真 TCP connect。测试用它注入 socketpair，
  // 这样"换节点重试"能在禁 bind 的环境里验证。
  std::function<std::expected<int, std::string>(const std::string &host,
                                               const std::string &port)>
      dialer;
};
std::unique_ptr<SqlConnection> make_remote(const RemoteOptions &options,
                                           std::string *error);

// 测试/嵌入用：已经建立好的一条连接（例如 socketpair 的一半）。
// 会先做协议握手（读 HELLO），失败返回 nullptr。
std::unique_ptr<SqlConnection>
make_remote_from_fd(int fd, std::string *error,
                    const std::string &label = "remote",
                    bool loose_cross_group_reads = false);

} // namespace client
