// tests/test_relation/test_catalog.cpp
//
// KVCatalog：sql::Catalog 的 KV 实现（元数据、DDL、表视图）。
#include "test_framework.h"

#include <memory>
#include <string>

#include "relation/kv_catalog.h"
#include "relation/table.h"
#include "relation_test_util.h"

namespace {

const sql::Identifier kDb("shop");

} // namespace

TEST(Catalog, CreateAndListDatabases) {
  auto engine = reltest::open_engine();
  sql::KVCatalog catalog(engine);
  CHECK(catalog.is_open());
  CHECK(catalog.current_database().empty());

  CHECK(catalog.create_database(kDb));
  CHECK(catalog.database_exists(kDb));
  CHECK(!catalog.create_database(kDb)); // 重名
  CHECK_EQ(catalog.list_databases().size(), size_t{1});

  // 标识符大小写不敏感：库名照样能查到
  CHECK(catalog.database_exists(sql::Identifier("SHOP")));
  CHECK(catalog.use_database(sql::Identifier("Shop")));
  // 保留调用方传入的原始大小写，但比较是大小写不敏感的
  CHECK_EQ(catalog.current_database().str(), std::string("Shop"));
  CHECK(catalog.current_database() == sql::Identifier("shop"));
  CHECK(!catalog.use_database(sql::Identifier("nope")));
}

TEST(Catalog, CreateTableAndReadSchemaBack) {
  auto engine = reltest::open_engine();
  sql::KVCatalog catalog(engine);
  CHECK(catalog.create_database(kDb));
  CHECK(catalog.create_table(kDb, reltest::make_users_schema()));

  CHECK(catalog.table_exists(kDb, sql::Identifier("users")));
  CHECK(!catalog.table_exists(kDb, sql::Identifier("missing")));
  CHECK(!catalog.create_table(kDb, reltest::make_users_schema())); // 重名
  CHECK(!catalog.create_table(sql::Identifier("nope"),
                              reltest::make_users_schema()));
  CHECK_EQ(catalog.list_tables(kDb).size(), size_t{1});

  auto schema = catalog.get_table_schema(kDb, sql::Identifier("users"));
  CHECK(schema.has_value());
  if (schema.has_value()) {
    CHECK_EQ(schema->table_name().str(), std::string("users"));
    CHECK_EQ(schema->column_count(), size_t{3});
    CHECK(schema->has_primary_key());
    CHECK_EQ(schema->primary_key_name().str(), std::string("id"));
  }
  CHECK(!catalog.get_table_schema(kDb, sql::Identifier("missing")).has_value());
}

TEST(Catalog, MetadataSurvivesNewCatalogInstance) {
  auto engine = reltest::open_engine();
  {
    sql::KVCatalog catalog(engine);
    CHECK(catalog.create_database(kDb));
    CHECK(catalog.create_table(kDb, reltest::make_users_schema()));
  }
  // 新实例读的是同一份 KV 元数据（没有内存缓存假象）
  sql::KVCatalog reopened(engine);
  CHECK(reopened.database_exists(kDb));
  CHECK(reopened.table_exists(kDb, sql::Identifier("users")));
}

TEST(Catalog, SchemaWithoutPrimaryKeyIsRejected) {
  auto engine = reltest::open_engine();
  sql::KVCatalog catalog(engine);
  CHECK(catalog.create_database(kDb));

  sql::TableSchema no_pk(sql::Identifier("logs"));
  no_pk.add_column(sql::Identifier("message"), sql::DataType::VARCHAR, 32u,
                   false, true);
  CHECK(!catalog.create_table(kDb, no_pk));
}

TEST(Catalog, NamesWithSeparatorsDoNotBreakMetadata) {
  auto engine = reltest::open_engine();
  sql::KVCatalog catalog(engine);
  CHECK(catalog.create_database(sql::Identifier("weird,db")));

  // 表名里带逗号/斜杠：长度前缀 framing 不会串行
  sql::TableSchema schema(sql::Identifier("a,b/c"));
  schema.add_column(sql::Identifier("id"), sql::DataType::INT, true, false);
  CHECK(catalog.create_table(sql::Identifier("weird,db"), schema));

  const auto tables = catalog.list_tables(sql::Identifier("weird,db"));
  CHECK_EQ(tables.size(), size_t{1});
  if (tables.size() == 1) {
    CHECK_EQ(tables[0].str(), std::string("a,b/c"));
  }
  CHECK(catalog
            .get_table_schema(sql::Identifier("weird,db"),
                              sql::Identifier("a,b/c"))
            .has_value());
}

TEST(Catalog, OpenTableReturnsUsableView) {
  auto engine = reltest::open_engine();
  sql::KVCatalog catalog(engine);
  CHECK(catalog.create_database(kDb));
  CHECK(catalog.create_table(kDb, reltest::make_users_schema()));
  CHECK(catalog.use_database(kDb));

  auto table = catalog.open_table(kDb, sql::Identifier("users"));
  CHECK(table.has_value());
  if (!table.has_value()) {
    return;
  }
  CHECK(table->insert(reltest::users_row(1, "a", 20)).has_value());
  CHECK_EQ(*table->row_count(), size_t{1});

  // 用当前数据库打开
  auto current = catalog.open_current_table(sql::Identifier("users"));
  CHECK(current.has_value());

  auto missing = catalog.open_table(kDb, sql::Identifier("nope"));
  CHECK(!missing.has_value());
  if (!missing.has_value()) {
    CHECK(missing.error().code == sql::RelErrorCode::TABLE_NOT_FOUND);
  }
}

TEST(Catalog, DropTableRemovesSchemaAndData) {
  auto engine = reltest::open_engine();
  sql::KVCatalog catalog(engine);
  CHECK(catalog.create_database(kDb));
  CHECK(catalog.create_table(kDb, reltest::make_users_schema()));

  {
    auto table = catalog.open_table(kDb, sql::Identifier("users"));
    CHECK(table.has_value());
    if (table.has_value()) {
      CHECK(table->insert(reltest::users_row(1, "a", 20)).has_value());
      CHECK(table->insert(reltest::users_row(2, "b", 21)).has_value());
    }
  }

  CHECK(catalog.drop_table(kDb, sql::Identifier("users")));
  CHECK(!catalog.table_exists(kDb, sql::Identifier("users")));
  CHECK_EQ(catalog.list_tables(kDb).size(), size_t{0});

  // 重新建同名表：数据必须是干净的（旧行没被留下）
  CHECK(catalog.create_table(kDb, reltest::make_users_schema()));
  auto table = catalog.open_table(kDb, sql::Identifier("users"));
  CHECK(table.has_value());
  if (table.has_value()) {
    CHECK_EQ(*table->row_count(), size_t{0});
  }
}

TEST(Catalog, DropDatabaseRemovesEverything) {
  auto engine = reltest::open_engine();
  sql::KVCatalog catalog(engine);
  CHECK(catalog.create_database(kDb));
  CHECK(catalog.create_table(kDb, reltest::make_users_schema()));
  {
    auto table = catalog.open_table(kDb, sql::Identifier("users"));
    CHECK(table.has_value());
    if (table.has_value()) {
      CHECK(table->insert(reltest::users_row(1, "a", 20)).has_value());
    }
  }

  CHECK(catalog.drop_database(kDb));
  CHECK(!catalog.database_exists(kDb));
  CHECK_EQ(catalog.list_databases().size(), size_t{0});
  CHECK(!catalog.get_table_schema(kDb, sql::Identifier("users")).has_value());
  // 数据 key 也应该被清掉：重建同名库/表后扫不到旧行
  CHECK(catalog.create_database(kDb));
  CHECK(catalog.create_table(kDb, reltest::make_users_schema()));
  auto table = catalog.open_table(kDb, sql::Identifier("users"));
  CHECK(table.has_value());
  if (table.has_value()) {
    CHECK_EQ(*table->row_count(), size_t{0});
  }
}

TEST(Catalog, UseDatabaseIsSessionStateOnly) {
  auto engine = reltest::open_engine();
  sql::KVCatalog catalog(engine);
  CHECK(catalog.create_database(kDb));
  CHECK(catalog.use_database(kDb));

  // 另一个实例不该继承"当前数据库"（那是会话状态，不落 KV）
  sql::KVCatalog other(engine);
  CHECK(other.current_database().empty());
}

TEST(Catalog, ClosedEngineReportsNotOpen) {
  auto engine = std::make_shared<kv::MockEngine>();
  sql::KVCatalog catalog(engine);
  CHECK(!catalog.is_open());
  CHECK(!catalog.create_database(kDb));

  auto table = catalog.open_table(kDb, sql::Identifier("users"));
  CHECK(!table.has_value());
  if (!table.has_value()) {
    CHECK(table.error().code == sql::RelErrorCode::NOT_OPEN);
  }
}

// ============================================================
// 统计信息（建表/建库时间、最后写入时间）
// ============================================================
TEST(Catalog, StatsRecordsCreationTimeWithInjectedClock) {
  auto engine = reltest::open_engine();
  int64_t now = 1000;
  sql::KVCatalog catalog(engine, [&now] { return now; });

  CHECK(catalog.create_database(kDb));
  now = 2000; // 建库时间应当在那一刻定格
  CHECK(catalog.create_table(kDb, reltest::make_users_schema()));

  auto db_stats = catalog.database_stats(kDb);
  CHECK(db_stats.has_value());
  if (db_stats.has_value()) {
    CHECK_EQ(db_stats->created_at, int64_t{1000});
  }
  auto table_stats = catalog.table_stats(kDb, sql::Identifier("users"));
  CHECK(table_stats.has_value());
  if (table_stats.has_value()) {
    CHECK_EQ(table_stats->created_at, int64_t{2000});
    CHECK_EQ(table_stats->last_write_at, int64_t{0}); // 还没写过
  }
}

TEST(Catalog, TouchTableUpdatesLastWriteTime) {
  auto engine = reltest::open_engine();
  int64_t now = 100;
  sql::KVCatalog catalog(engine, [&now] { return now; });
  CHECK(catalog.create_database(kDb));
  CHECK(catalog.create_table(kDb, reltest::make_users_schema()));

  now = 500;
  CHECK(catalog.touch_table(kDb, sql::Identifier("users")));
  auto stats = catalog.table_stats(kDb, sql::Identifier("users"));
  CHECK(stats.has_value());
  if (stats.has_value()) {
    CHECK_EQ(stats->created_at, int64_t{100});    // 建表时间不变
    CHECK_EQ(stats->last_write_at, int64_t{500}); // 写入时间更新了
  }
  // 表不存在时不能瞎写
  CHECK(!catalog.touch_table(kDb, sql::Identifier("nope")));
}

TEST(Catalog, StatsSurviveReopenAndAreDroppedWithTheTable) {
  auto engine = reltest::open_engine();
  int64_t now = 42;
  {
    sql::KVCatalog catalog(engine, [&now] { return now; });
    CHECK(catalog.create_database(kDb));
    CHECK(catalog.create_table(kDb, reltest::make_users_schema()));
    now = 99;
    CHECK(catalog.touch_table(kDb, sql::Identifier("users")));
  }
  // 新实例从 KV 读回
  {
    sql::KVCatalog reopened(engine, [&now] { return now; });
    auto stats = reopened.table_stats(kDb, sql::Identifier("users"));
    CHECK(stats.has_value());
    if (stats.has_value()) {
      CHECK_EQ(stats->created_at, int64_t{42});
      CHECK_EQ(stats->last_write_at, int64_t{99});
    }
    // DROP TABLE 要把统计一起删掉：重新建表后 last_write_at 必须回到 0
    CHECK(reopened.drop_table(kDb, sql::Identifier("users")));
    now = 123;
    CHECK(reopened.create_table(kDb, reltest::make_users_schema()));
    auto fresh = reopened.table_stats(kDb, sql::Identifier("users"));
    CHECK(fresh.has_value());
    if (fresh.has_value()) {
      CHECK_EQ(fresh->created_at, int64_t{123});
      CHECK_EQ(fresh->last_write_at, int64_t{0}); // 没有残留旧值
    }
  }
}

TEST(Catalog, DropDatabaseRemovesStats) {
  auto engine = reltest::open_engine();
  int64_t now = 7;
  sql::KVCatalog catalog(engine, [&now] { return now; });
  CHECK(catalog.create_database(kDb));
  CHECK(catalog.create_table(kDb, reltest::make_users_schema()));
  now = 70;
  CHECK(catalog.touch_table(kDb, sql::Identifier("users")));
  CHECK(catalog.drop_database(kDb));

  // 重建同名库表：统计必须是全新的（旧的 dbstats/tablestats 已被清掉）
  now = 777;
  CHECK(catalog.create_database(kDb));
  CHECK(catalog.create_table(kDb, reltest::make_users_schema()));
  auto db_stats = catalog.database_stats(kDb);
  CHECK(db_stats.has_value());
  if (db_stats.has_value()) {
    CHECK_EQ(db_stats->created_at, int64_t{777});
  }
  auto table_stats = catalog.table_stats(kDb, sql::Identifier("users"));
  CHECK(table_stats.has_value());
  if (table_stats.has_value()) {
    CHECK_EQ(table_stats->last_write_at, int64_t{0});
  }
}

TEST(Catalog, StatsForMissingObjectsReportErrors) {
  auto engine = reltest::open_engine();
  sql::KVCatalog catalog(engine);
  CHECK(catalog.create_database(kDb));

  auto db_stats = catalog.database_stats(sql::Identifier("nope"));
  CHECK(!db_stats.has_value());
  if (!db_stats.has_value()) {
    CHECK(db_stats.error().code == sql::RelErrorCode::NOT_FOUND);
  }
  auto table_stats = catalog.table_stats(kDb, sql::Identifier("nope"));
  CHECK(!table_stats.has_value());
  if (!table_stats.has_value()) {
    CHECK(table_stats.error().code == sql::RelErrorCode::TABLE_NOT_FOUND);
  }
}
