// tests/test_server/test_client.cpp
//
// 客户端层：`client::RemoteConnection`（真协议客户端）+ 共用 REPL。
// 这条用例把"远程客户端连服务器跑 SQL"整条链路都过一遍 —— 和 `sqldb-client`
// 用的是同一份代码（client/connection.cpp + client/repl.cpp）。
#include "test_framework.h"

#include "client/connection.h"
#include "client/repl.h"
#include "storage_helper.h"

#include <cstdio>
#include <string>
#include <vector>

TEST(RemoteClient, ExecutesStatementsThroughTheProtocol) {
  srvtest::RunningServer server;
  if (!server.start()) {
    // 沙箱不允许 bind：跳过（见 test_e2e.cpp 的说明）
    fmt::print(stderr, "[skip] server start failed: {}\n", server.last_error());
    return;
  }

  client::RemoteOptions options;
  options.host = "127.0.0.1";
  options.port = std::to_string(server.port());
  std::string error;
  auto connection = client::make_remote(options, &error);
  CHECK(connection != nullptr);
  if (connection == nullptr) {
    fmt::print(stderr, "[skip] connect failed: {}\n", error);
    return;
  }

  // 建表 + 多行插入（服务器配置里 default_database=shop）
  auto create =
      connection->execute("CREATE TABLE t (id INT PRIMARY KEY, v INT)");
  CHECK(create.ok);
  CHECK(!create.has_rows);
  CHECK(!create.is_write); // DDL

  auto inserted =
      connection->execute("INSERT INTO t (id, v) VALUES (1, 10), (2, 20)");
  CHECK(inserted.ok);
  CHECK(inserted.is_write);
  CHECK_EQ(inserted.affected_rows, uint64_t{2});

  auto selected = connection->execute("SELECT id, v FROM t ORDER BY id DESC");
  CHECK(selected.ok);
  CHECK(selected.has_rows);
  CHECK_EQ(selected.columns.size(), size_t{2});
  CHECK_EQ(selected.rows.size(), size_t{2});
  if (selected.rows.size() == 2) {
    CHECK_EQ(selected.rows[0][0].text, std::string("2"));
    CHECK_EQ(selected.rows[1][1].text, std::string("10"));
  }

  // 错误：消息 + span（客户端自己渲染 caret）
  auto missing = connection->execute("SELECT * FROM nope");
  CHECK(!missing.ok);
  CHECK(missing.error_message.find("nope") != std::string::npos);
  CHECK(sspan_valid(missing.error_span));

  // 事务状态随 OK 帧回来（提示符 * 靠它）
  CHECK(!connection->in_transaction());
  CHECK(connection->execute("BEGIN").ok);
  CHECK(connection->execute("SELECT id FROM t").ok);
  CHECK(connection->in_transaction());
  CHECK(connection->execute("ROLLBACK").ok);
  CHECK(!connection->in_transaction());
}

TEST(RemoteClient, MetadataCommandsWorkOverTheWire) {
  srvtest::RunningServer server;
  if (!server.start()) {
    fmt::print(stderr, "[skip] server start failed: {}\n", server.last_error());
    return;
  }
  client::RemoteOptions options;
  options.host = "127.0.0.1";
  options.port = std::to_string(server.port());
  std::string error;
  auto connection = client::make_remote(options, &error);
  CHECK(connection != nullptr);
  if (connection == nullptr) {
    return;
  }
  CHECK(connection->supports_metadata());

  // RunningServer 已经建好 shop.users(id, name, age)
  const auto databases = connection->databases();
  bool found_shop = false;
  for (const auto &db : databases) {
    if (db.name == "shop") {
      found_shop = true;
    }
  }
  CHECK(found_shop);

  const auto tables = connection->tables("shop");
  bool found_users = false;
  for (const auto &table : tables) {
    if (table.name == "users") {
      found_users = true;
      CHECK_EQ(table.column_count, size_t{3});
      CHECK_EQ(table.primary_key, std::string("id"));
    }
  }
  CHECK(found_users);

  const auto schema = connection->table_schema("users", "shop");
  CHECK(schema.has_value());
  if (schema.has_value()) {
    CHECK_EQ(schema->column_count(), size_t{3});
    CHECK(schema->has_primary_key());
  }
  // 表不存在 -> nullopt（服务器回 ERROR 帧）
  CHECK(!connection->table_schema("nope", "shop").has_value());

  // 元命令也走通了：REPL 跑 \l / \dt / \d users
  FILE *sink = std::tmpfile();
  CHECK(sink != nullptr);
  if (sink == nullptr) {
    return;
  }
  client::ReplOptions repl;
  repl.out = sink;
  repl.err = sink;
  repl.colors = false;
  const std::string script = "\\l\n\\dt\n\\d users\n\\c shop\n";
  CHECK_EQ(client::run_text(*connection, script, repl), 0);

  std::fflush(sink);
  std::rewind(sink);
  std::string output;
  char buffer[4096];
  size_t got = 0;
  while ((got = std::fread(buffer, 1, sizeof(buffer), sink)) > 0) {
    output.append(buffer, got);
  }
  std::fclose(sink);
  CHECK(output.find("shop") != std::string::npos);        // \l / \dt
  CHECK(output.find("users") != std::string::npos);       // \dt / \d
  CHECK(output.find("PRIMARY KEY") != std::string::npos); // \d 的结构表
}

TEST(RemoteClient, SharedReplRunsAScriptOverTheWire) {
  srvtest::RunningServer server;
  if (!server.start()) {
    return;
  }
  client::RemoteOptions options;
  options.host = "127.0.0.1";
  options.port = std::to_string(server.port());
  std::string error;
  auto connection = client::make_remote(options, &error);
  CHECK(connection != nullptr);
  if (connection == nullptr) {
    return;
  }

  // REPL 的输出重定向到临时文件，然后跑一段脚本（和 sqldb-client 同一条路径）
  FILE *sink = std::tmpfile();
  CHECK(sink != nullptr);
  if (sink == nullptr) {
    return;
  }
  client::ReplOptions repl;
  repl.out = sink;
  repl.err = sink;
  repl.colors = false;
  const std::string script =
      "CREATE TABLE r (id INT PRIMARY KEY, s VARCHAR(8));\n"
      "INSERT INTO r (id, s) VALUES (1, '');\n"
      "INSERT INTO r (id, s) VALUES (2, NULL);\n"
      "SELECT id, s FROM r ORDER BY id;\n";
  CHECK_EQ(client::run_text(*connection, script, repl), 0);

  std::fflush(sink);
  std::rewind(sink);
  std::string output;
  char buffer[4096];
  size_t got = 0;
  while ((got = std::fread(buffer, 1, sizeof(buffer), sink)) > 0) {
    output.append(buffer, got);
  }
  std::fclose(sink);

  CHECK(output.find("OK") != std::string::npos);
  CHECK(output.find("id") != std::string::npos);       // 列名
  CHECK(output.find("NULL") != std::string::npos);     // NULL 单元格
  CHECK(output.find("(2 rows)") != std::string::npos); // 两行
}

// ============================================================
// TAB 补全：`completion_candidates` 不碰 readline，可以直接单测
// （readline 只是把它的结果喂给 rl_completion_entry_function）。
// connection 传 nullptr -> 只出关键字/元命令，不查表名。
// ============================================================
namespace {

// 只用来验证补全的"表名来源"：不连任何东西，tables() 返回一个固定表。
class FakeMetadataConnection : public client::SqlConnection {
public:
  client::Outcome execute(const std::string &sql) override {
    (void)sql;
    return {};
  }
  std::string current_database() const override { return "shop"; }
  bool in_transaction() const override { return false; }
  bool supports_metadata() const override { return true; }
  std::vector<client::TableMeta> tables(const std::string &db) override {
    (void)db;
    client::TableMeta table;
    table.name = "widgets";
    return {table};
  }
  std::string description() const override { return "fake"; }
};

bool contains(const std::vector<std::string> &list, const std::string &value) {
  for (const std::string &item : list) {
    if (item == value) {
      return true;
    }
  }
  return false;
}

bool is_sorted_unique(const std::vector<std::string> &list) {
  for (size_t i = 1; i < list.size(); ++i) {
    if (!(list[i - 1] < list[i])) {
      return false;
    }
  }
  return true;
}

} // namespace

TEST(ReplCompletion, KeywordPrefixFiltersAndIncludesSqlVocabulary) {
  const auto candidates = client::completion_candidates("sel", nullptr);
  CHECK(contains(candidates, "select"));
  // 前缀过滤：不该出现不以 "sel" 开头的词
  for (const std::string &item : candidates) {
    CHECK_EQ(item.compare(0, 3, "sel"), 0);
  }
  CHECK(is_sorted_unique(candidates));

  // 空前缀 = 全部关键字（至少覆盖核心词汇），仍然排序去重
  const auto all = client::completion_candidates("", nullptr);
  CHECK(is_sorted_unique(all));
  CHECK(contains(all, "create"));
  CHECK(contains(all, "database"));
  CHECK(contains(all, "rollback"));

  // 大小写：readline 的 rl_line_buffer 保留用户输入原样；前缀按字节比较，
  // 大写前缀当前不补全（提示是列表全小写）——这里固化成"不炸"的行为
  CHECK(client::completion_candidates("SEL", nullptr).empty());
}

TEST(ReplCompletion, MetaCommandsAreCompletedSeparately) {
  const auto candidates = client::completion_candidates("\\d", nullptr);
  CHECK(contains(candidates, "\\dt"));
  // "\d" 本身等于前缀，不重复补
  CHECK_FALSE(contains(candidates, "\\d"));
  for (const std::string &item : candidates) {
    CHECK_EQ(item[0], '\\');
    CHECK_EQ(item.compare(0, 2, "\\d"), 0);
  }

  const auto all = client::completion_candidates("\\", nullptr);
  CHECK(contains(all, "\\l"));
  CHECK(contains(all, "\\c"));
  CHECK(contains(all, "\\begin"));
  CHECK(contains(all, "\\commit"));
  // 元命令列表里不该混入 SQL 关键字
  CHECK_FALSE(contains(all, "select"));
}

TEST(ReplCompletion, CompletesOnlyTheCurrentWord) {
  // 补的是光标前的"当前词"：前面的字符不参与前缀匹配。
  // '(' 是词边界：这里补的是 "sel"，所以只出 "select"。
  const auto after_paren =
      client::completion_candidates("INSERT INTO t VALUES (sel", nullptr);
  CHECK(contains(after_paren, "select"));

  // 完全相同的词不再补（否则按 TAB 没有任何反馈）
  CHECK(client::completion_candidates("select", nullptr).empty());

  // 查表名：元信息可用时，当前词也能补出表名（和关键字一起排序去重）
  FakeMetadataConnection fake;
  const auto table_prefix =
      client::completion_candidates("select * from wi", &fake);
  CHECK(contains(table_prefix, "widgets"));
  for (const std::string &item : table_prefix) {
    CHECK_EQ(item.compare(0, 2, "wi"), 0);
  }
  CHECK(is_sorted_unique(table_prefix));

  // 没匹配上就返回空（readline 会保持原样）
  CHECK(client::completion_candidates("select * from zzz", &fake).empty());

  // 换个词以后，前面出现过的表名不再参与（补的是当前词，不是整行）
  CHECK(client::completion_candidates("select widgets from zz", &fake).empty());

  // 当前词是空的（行尾是空白）：列出全部候选（关键字 + 表名）——这是 TAB
  // 在空词上的标准行为（"看看有什么可选"）。
  const auto empty_word =
      client::completion_candidates("select widgets from ", &fake);
  CHECK(contains(empty_word, "widgets"));
  CHECK(contains(empty_word, "select"));
  CHECK(is_sorted_unique(empty_word));
}
