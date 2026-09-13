// tests/test_session/test_metadata.cpp
//
// 元信息（元命令的数据来源）：库列表、表列表、schema、行数与统计。
#include "test_framework.h"

#include <memory>
#include <string>

#include "session_test_util.h"

namespace {

// 假时钟：测试里想什么时候"现在"就什么时候
int64_t g_now = 1000;

int64_t fake_clock() { return g_now; }

} // namespace

TEST(Metadata, DatabasesCarryTableCountAndCreationTime) {
  auto engine = sess_test::open_engine();
  session::Session session(engine, fake_clock);
  g_now = 1000;
  CHECK(sess_test::bootstrap(session, 0)); // 建 shop 库 + users 表
  g_now = 2000;
  session::SessionError error;
  CHECK(sess_test::run(session, "CREATE DATABASE other", &error) != nullptr);

  const auto databases = session.databases();
  CHECK_EQ(databases.size(), size_t{2});
  if (databases.size() != 2) {
    return;
  }
  // list_databases 的顺序就是创建顺序
  CHECK_EQ(databases[0].name.str(), std::string("shop"));
  CHECK_EQ(databases[0].table_count, size_t{1});
  CHECK_EQ(databases[0].created_at, int64_t{1000});
  CHECK(databases[0].is_current); // bootstrap 里 USE 了 shop

  CHECK_EQ(databases[1].name.str(), std::string("other"));
  CHECK_EQ(databases[1].table_count, size_t{0});
  CHECK_EQ(databases[1].created_at, int64_t{2000});
  CHECK(!databases[1].is_current);
}

TEST(Metadata, TablesCarrySchemaSummaryAndStats) {
  auto engine = sess_test::open_engine();
  session::Session session(engine, fake_clock);
  g_now = 10;
  CHECK(sess_test::bootstrap(session, 0));
  g_now = 20;
  session::SessionError error;
  CHECK(sess_test::run(session,
                       "INSERT INTO users (id, name, age) VALUES (1, 'a', 10)",
                       &error) != nullptr);
  CHECK(sess_test::run(session,
                       "INSERT INTO users (id, name, age) VALUES (2, 'b', 20)",
                       &error) != nullptr);

  const auto tables = session.tables();
  CHECK_EQ(tables.size(), size_t{1});
  if (tables.size() != 1) {
    return;
  }
  const session::TableInfo &info = tables[0];
  CHECK_EQ(info.name.str(), std::string("users"));
  CHECK_EQ(info.column_count, size_t{3});
  CHECK_EQ(info.primary_key.str(), std::string("id"));
  CHECK_EQ(info.row_count, size_t{2});       // 行数是现算的
  CHECK_EQ(info.created_at, int64_t{10});    // 建表时间
  CHECK_EQ(info.last_write_at, int64_t{20}); // session 在写成功后更新时间
}

TEST(Metadata, LastWriteTimeIsNotTouchedBySelectOrFailedWrite) {
  auto engine = sess_test::open_engine();
  session::Session session(engine, fake_clock);
  g_now = 10;
  CHECK(sess_test::bootstrap(session, 0)); // 只建库建表，还没写数据

  g_now = 50;
  session::SessionError error;
  CHECK(sess_test::run(session, "SELECT * FROM users", &error) != nullptr);
  // 写语句但影响 0 行：也不算"改过表"
  CHECK(sess_test::run(session, "DELETE FROM users WHERE id = 999", &error) !=
        nullptr);
  auto tables = session.tables();
  CHECK_EQ(tables.size(), size_t{1});
  if (!tables.empty()) {
    CHECK_EQ(tables[0].last_write_at, int64_t{0}); // 还没真正写过
  }

  // 真改了才更新
  CHECK(sess_test::run(session,
                       "INSERT INTO users (id, name, age) VALUES (1, 'a', 10)",
                       &error) != nullptr);
  tables = session.tables();
  if (!tables.empty()) {
    CHECK_EQ(tables[0].last_write_at, int64_t{50});
    CHECK_EQ(tables[0].row_count, size_t{1});
  }
}

TEST(Metadata, TablesCanBeListedForAnotherDatabase) {
  auto engine = sess_test::open_engine();
  session::Session session(engine, fake_clock);
  CHECK(sess_test::bootstrap(session, 1));
  session::SessionError error;
  CHECK(sess_test::run(session, "CREATE DATABASE db2", &error) != nullptr);
  CHECK(sess_test::run(session, "USE db2", &error) != nullptr);
  CHECK(sess_test::run(session, "CREATE TABLE t2 (id INT PRIMARY KEY)",
                       &error) != nullptr);

  // 不带参数 = 当前库（db2）
  const auto current = session.tables();
  CHECK_EQ(current.size(), size_t{1});
  if (!current.empty()) {
    CHECK_EQ(current[0].name.str(), std::string("t2"));
  }
  // 指定库
  const auto shop = session.tables(sql::Identifier("shop"));
  CHECK_EQ(shop.size(), size_t{1});
  if (!shop.empty()) {
    CHECK_EQ(shop[0].name.str(), std::string("users"));
  }
  // 不存在的库 -> 空列表
  CHECK(session.tables(sql::Identifier("nope")).empty());
}

TEST(Metadata, TableSchemaLookup) {
  auto engine = sess_test::open_engine();
  session::Session session(engine, fake_clock);
  CHECK(sess_test::bootstrap(session, 0));

  auto schema = session.table_schema(sql::Identifier("users"));
  CHECK(schema.has_value());
  if (schema.has_value()) {
    CHECK_EQ(schema->table_name().str(), std::string("users"));
    CHECK_EQ(schema->column_count(), size_t{3});
    CHECK_EQ(schema->primary_key_name().str(), std::string("id"));
    CHECK(schema->to_string_table().find("VARCHAR(32)") != std::string::npos);
  }
  // 指定库
  CHECK(session.table_schema(sql::Identifier("users"), sql::Identifier("shop"))
            .has_value());
  // 不存在
  CHECK(!session.table_schema(sql::Identifier("nope")).has_value());
  CHECK(!session.table_schema(sql::Identifier("")).has_value());
}

TEST(Metadata, DroppedTableDisappearsFromMetadata) {
  auto engine = sess_test::open_engine();
  session::Session session(engine, fake_clock);
  CHECK(sess_test::bootstrap(session, 1));
  session::SessionError error;
  CHECK(sess_test::run(session, "DROP TABLE users", &error) != nullptr);

  CHECK(session.tables().empty());
  CHECK(!session.table_schema(sql::Identifier("users")).has_value());
  const auto databases = session.databases();
  CHECK_EQ(databases.size(), size_t{1});
  if (!databases.empty()) {
    CHECK_EQ(databases[0].table_count, size_t{0});
  }
}

TEST(Metadata, EmptySessionReportsNothing) {
  auto engine = sess_test::open_engine();
  session::Session session(engine, fake_clock);
  CHECK(session.databases().empty());
  CHECK(session.tables().empty()); // 没有当前库
  CHECK(!session.table_schema(sql::Identifier("users")).has_value());
}
