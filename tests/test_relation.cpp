// tests/test_sql_layer.cpp
#include <iostream>

#include "relation/sql_relation.h"
#include "storage/mock_engine/mock_engine.h"

using namespace sql;

void print_separator(const std::string& title = "") {
  std::cout << "\n" << std::string(60, '=') << std::endl;
  if (!title.empty()) {
    std::cout << "  " << title << std::endl;
    std::cout << std::string(60, '=') << std::endl;
  }
}

int main() {
  std::cout << "🧪 SQL Layer Test" << std::endl;
  std::cout << std::string(50, '=') << std::endl;

  // 创建 KV 引擎
  auto engine = std::make_shared<kv::MockEngine>();
  engine->open_database(kv::DatabaseOptions().set_path("./sql_test"));

  // 创建数据库管理器
  DatabaseManager db_manager(engine);

  // ============================================================
  // 1. 创建数据库
  // ============================================================
  print_separator("创建数据库");

  db_manager.create_database("testdb");
  db_manager.create_database("blogdb");

  auto dbs = db_manager.list_databases();
  std::cout << "数据库列表: ";
  for (const auto& db : dbs) {
    std::cout << db << " ";
  }
  std::cout << std::endl;

  // ============================================================
  // 2. 打开数据库
  // ============================================================
  print_separator("打开数据库");

  auto db = db_manager.open_database("testdb");
  if (!db) {
    std::cerr << "❌ 打开数据库失败" << std::endl;
    return 1;
  }
  std::cout << "✅ 打开数据库: " << db->name() << std::endl;

  // ============================================================
  // 3. 创建表
  // ============================================================
  print_separator("创建表");

  TableSchema user_schema("users");
  user_schema.add_column("id", DataType::INT, true, false);  // PK, NOT NULL
  user_schema.add_column("name", DataType::VARCHAR, false, false);
  user_schema.add_column("age", DataType::INT, false, false);
  user_schema.add_column("email", DataType::VARCHAR, false, true);

  if (db->create_table(user_schema)) {
    std::cout << "✅ 创建表: users" << std::endl;
  }

  TableSchema post_schema("posts");
  post_schema.add_column("id", DataType::INT, true, false);
  post_schema.add_column("title", DataType::VARCHAR, false, false);
  post_schema.add_column("content", DataType::TEXT, false, false);
  post_schema.add_column("author_id", DataType::INT, false, false);

  if (db->create_table(post_schema)) {
    std::cout << "✅ 创建表: posts" << std::endl;
  }

  // 列出表
  auto tables = db->list_tables();
  std::cout << "表列表: ";
  for (const auto& t : tables) {
    std::cout << t << " ";
  }
  std::cout << std::endl;

  // ============================================================
  // 4. 获取表
  // ============================================================
  print_separator("获取表");

  auto users = db->get_table("users");
  if (!users) {
    std::cerr << "❌ 获取表失败" << std::endl;
    return 1;
  }
  std::cout << "✅ 获取表: " << users->name() << std::endl;
  std::cout << "  列: ";
  for (const auto& col : users->schema().columns()) {
    std::cout << col.name << "(" << data_type_name(col.type) << ") ";
  }
  std::cout << std::endl;

  // ============================================================
  // 5. 插入数据
  // ============================================================
  print_separator("插入数据");

  auto row1 = sql::row()
                  .set("id", 1)
                  .set("name", "Alice")
                  .set("age", 25)
                  .set("email", "alice@example.com")
                  .build(user_schema);

  if (users->insert(row1)) {
    std::cout << "✅ 插入用户: Alice" << std::endl;
  }

  auto row2 = row()
                  .set("id", 2)
                  .set("name", "Bob")
                  .set("age", 30)
                  .set("email", "bob@example.com")
                  .build(user_schema);

  if (users->insert(row2)) {
    std::cout << "✅ 插入用户: Bob" << std::endl;
  }

  auto row3 = row()
                  .set("id", 3)
                  .set("name", "Charlie")
                  .set("age", 35)
                  .set_null("email")
                  .build(user_schema);

  if (users->insert(row3)) {
    std::cout << "✅ 插入用户: Charlie (email NULL)" << std::endl;
  }

  // ============================================================
  // 6. 查询数据
  // ============================================================
  print_separator("查询数据");

  auto result = users->get(Value(2));
  if (result) {
    std::cout << "✅ 查询 id=2: " << result->to_string() << std::endl;
  }

  // ============================================================
  // 7. 全表扫描
  // ============================================================
  print_separator("全表扫描");

  auto all_rows = users->scan_all();
  std::cout << "✅ 所有用户 (" << all_rows.size() << " 行):" << std::endl;
  for (const auto& row : all_rows) {
    std::cout << "  " << row.to_string() << std::endl;
  }

  // ============================================================
  // 8. 更新数据
  // ============================================================
  print_separator("更新数据");

  auto updated = row()
                     .set("id", 1)
                     .set("name", "Alice Updated")
                     .set("age", 26)
                     .set("email", "alice_new@example.com")
                     .build(user_schema);

  if (users->update(Value(1), updated)) {
    std::cout << "✅ 更新 id=1: Alice → Alice Updated" << std::endl;
  }

  // 使用赋值方式更新
  std::vector<std::pair<std::string, Value>> assignments = {
      {"age", Value(31)}, {"email", Value("bob_new@example.com")}};
  if (users->update(Value(2), assignments)) {
    std::cout << "✅ 更新 id=2: age=31, email=bob_new@example.com" << std::endl;
  }

  // ============================================================
  // 9. 删除数据
  // ============================================================
  print_separator("删除数据");

  if (users->remove(Value(3))) {
    std::cout << "✅ 删除 id=3: Charlie" << std::endl;
  }

  // ============================================================
  // 10. 最终数据
  // ============================================================
  print_separator("最终数据");

  all_rows = users->scan_all();
  std::cout << "✅ 最终用户 (" << all_rows.size() << " 行):" << std::endl;
  for (const auto& row : all_rows) {
    std::cout << "  " << row.to_string() << std::endl;
  }

  std::cout << "\n用户数: " << users->row_count() << std::endl;

  // ============================================================
  // 11. 测试失败用例
  // ============================================================
  print_separator("失败用例测试");

  // 插入无效行（列数不匹配）
  Row invalid_row({Value(99), Value("Invalid")});
  if (!users->insert(invalid_row)) {
    std::cout << "✅ 正确拒绝无效行（列数不匹配）" << std::endl;
  }

  // 插入 NULL 到 NOT NULL 列
  auto null_row = row()
                      .set("id", 99)
                      .set_null("name")  // name 是 NOT NULL
                      .build(user_schema);
  if (!users->insert(null_row)) {
    std::cout << "✅ 正确拒绝 NULL 到 NOT NULL 列" << std::endl;
  }

  // ============================================================
  // 12. 清理
  // ============================================================
  print_separator("清理");

  db->drop_table("users");
  db->drop_table("posts");
  db_manager.drop_database("testdb");
  db_manager.drop_database("blogdb");

  dbs = db_manager.list_databases();
  std::cout << "剩余数据库: ";
  for (const auto& db : dbs) {
    std::cout << db << " ";
  }
  std::cout << std::endl;

  std::cout << "\n✅ SQL Layer 测试完成!" << std::endl;

  return 0;
}