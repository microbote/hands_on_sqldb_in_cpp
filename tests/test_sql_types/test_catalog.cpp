// tests/test_sql_types/test_catalog.cpp
//
// Catalog / DatabaseView 是纯逻辑元数据接口，这里用内存实现验证
// 接口契约（Identifier 传参、大小写不敏感、可选 schema）。
#include "test_framework.h"
#include "sql_types/catalog.h"
#include "sql_types/db_view.h"

#include <unordered_map>

using namespace sql;

namespace {

class MemoryCatalog : public Catalog {
 public:
  bool is_open() const override { return !current_db_.empty(); }
  Identifier current_database() const override { return current_db_; }

  bool database_exists(const Identifier& db_name) const override {
    return databases_.find(db_name) != databases_.end();
  }

  std::vector<Identifier> list_databases() const override {
    std::vector<Identifier> names;
    for (const auto& [name, tables] : databases_) {
      (void)tables;
      names.push_back(name);
    }
    return names;
  }

  bool table_exists(const Identifier& db_name,
                    const Identifier& table_name) const override {
    auto db = databases_.find(db_name);
    return db != databases_.end() &&
           db->second.find(table_name) != db->second.end();
  }

  std::vector<Identifier> list_tables(const Identifier& db_name) const override {
    std::vector<Identifier> names;
    auto db = databases_.find(db_name);
    if (db != databases_.end()) {
      for (const auto& [name, schema] : db->second) {
        (void)schema;
        names.push_back(name);
      }
    }
    return names;
  }

  std::optional<TableSchema> get_table_schema(
      const Identifier& db_name, const Identifier& table_name) const override {
    auto db = databases_.find(db_name);
    if (db == databases_.end()) {
      return std::nullopt;
    }
    auto table = db->second.find(table_name);
    if (table == db->second.end()) {
      return std::nullopt;
    }
    return table->second;
  }

  bool create_database(const Identifier& db_name) override {
    const bool created = databases_.emplace(db_name, TableMap{}).second;
    if (created && current_db_.empty()) {
      current_db_ = db_name;   // 简化：建库即进入该库
    }
    return created;
  }

  bool drop_database(const Identifier& db_name) override {
    return databases_.erase(db_name) > 0;
  }

  bool create_table(const Identifier& db_name,
                    const TableSchema& schema) override {
    auto db = databases_.find(db_name);
    if (db == databases_.end()) {
      return false;
    }
    return db->second.emplace(schema.table_name(), schema).second;
  }

  bool drop_table(const Identifier& db_name,
                  const Identifier& table_name) override {
    auto db = databases_.find(db_name);
    if (db == databases_.end()) {
      return false;
    }
    return db->second.erase(table_name) > 0;
  }

 private:
  using TableMap = std::unordered_map<Identifier, TableSchema, IdentifierHash>;
  std::unordered_map<Identifier, TableMap, IdentifierHash> databases_;
  Identifier current_db_;
};

TableSchema make_users() {
  TableSchema s;
  s.set_name(Identifier("users"));
  s.add_column(Identifier("id"), DataType::INT, true, false);
  s.add_column(Identifier("name"), DataType::VARCHAR, false, true);
  return s;
}

}  // namespace

TEST(Catalog, DatabaseLifecycle) {
  MemoryCatalog catalog;
  CHECK(!catalog.database_exists(Identifier("shop")));
  CHECK(catalog.create_database(Identifier("shop")));
  CHECK(catalog.database_exists(Identifier("shop")));
  CHECK(!catalog.create_database(Identifier("shop")));  // 重复创建失败

  CHECK_EQ(catalog.list_databases().size(), 1);
  CHECK(catalog.drop_database(Identifier("shop")));
  CHECK(!catalog.database_exists(Identifier("shop")));
  CHECK(!catalog.drop_database(Identifier("shop")));
}

TEST(Catalog, NameLookupIsCaseInsensitive) {
  MemoryCatalog catalog;
  catalog.create_database(Identifier("Shop"));
  catalog.create_table(Identifier("Shop"), make_users());

  CHECK(catalog.database_exists(Identifier("shop")));
  CHECK(catalog.database_exists(Identifier("SHOP")));
  CHECK(catalog.table_exists(Identifier("shop"), Identifier("USERS")));
  CHECK(catalog.table_exists(Identifier("SHOP"), Identifier("users")));
  CHECK(!catalog.table_exists(Identifier("shop"), Identifier("orders")));
  CHECK(!catalog.table_exists(Identifier("other"), Identifier("users")));
}

TEST(Catalog, TableLifecycleAndSchemaLookup) {
  MemoryCatalog catalog;
  catalog.create_database(Identifier("shop"));

  CHECK(!catalog.table_exists(Identifier("shop"), Identifier("users")));
  CHECK(catalog.create_table(Identifier("shop"), make_users()));
  CHECK(catalog.table_exists(Identifier("shop"), Identifier("users")));

  auto schema = catalog.get_table_schema(Identifier("shop"), Identifier("users"));
  CHECK(schema.has_value());
  if (schema.has_value()) {
    CHECK(schema->table_name() == Identifier("users"));
    CHECK_EQ(schema->column_count(), 2);
    CHECK(schema->has_primary_key());
  }

  CHECK_EQ(catalog.list_tables(Identifier("SHOP")).size(), 1);
  CHECK(catalog.drop_table(Identifier("shop"), Identifier("users")));
  CHECK(!catalog.get_table_schema(Identifier("shop"), Identifier("users"))
             .has_value());
}

TEST(DatabaseView, DelegatesToCatalog) {
  MemoryCatalog catalog;
  catalog.create_database(Identifier("shop"));
  catalog.create_table(Identifier("shop"), make_users());

  DatabaseView view(catalog, Identifier("Shop"));
  CHECK(view.table_exists(Identifier("users")));
  CHECK(!view.table_exists(Identifier("orders")));
  CHECK_EQ(view.list_tables().size(), 1);

  auto schema = view.get_table_schema(Identifier("Users"));
  CHECK(schema.has_value());
  if (schema.has_value()) {
    CHECK_EQ(schema->column_count(), 2);
  }
  CHECK(!view.get_table_schema(Identifier("missing")).has_value());
}
