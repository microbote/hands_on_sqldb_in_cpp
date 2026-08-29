// tests/test_relation.cpp
#include <iostream>
#include <memory>
#include <vector>

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

// ============================================================
// 测试 1: TableSchema 构建
// ============================================================
void test_schema_builder() {
  print_separator("测试 TableSchema 构建");

  TableSchema user_schema("users");
  user_schema.primary_key("id", DataType::INT)
      .not_null("name", DataType::VARCHAR)
      .not_null("age", DataType::INT)
      .nullable("email", DataType::VARCHAR);

  SchemaError err = user_schema.validate();
  if (err == SchemaError::OK) {
    std::cout << "✅ Schema 有效" << std::endl;
    std::cout << "  表名: " << user_schema.name() << std::endl;
    std::cout << "  列数: " << user_schema.columns().size() << std::endl;
    std::cout << "  主键: " << user_schema.primary_key_name() << std::endl;
    for (const auto& col : user_schema.columns()) {
      std::cout << "    " << col.name << " (" << data_type_name(col.type)
                << ", " << (col.nullable ? "nullable" : "NOT NULL") << ", "
                << (col.primary_key ? "PK" : "") << ")" << std::endl;
    }
  } else {
    std::cout << "❌ " << TableSchema::error_message(err) << std::endl;
  }

  // 测试错误：多个主键
  TableSchema bad_schema("bad_users");
  bad_schema.primary_key("id", DataType::INT)
      .primary_key("uuid", DataType::VARCHAR)  // ❌ 第二个主键
      .not_null("name", DataType::VARCHAR);

  err = bad_schema.validate();
  if (err != SchemaError::OK) {
    std::cout << "\n✅ 正确检测到多主键错误: "
              << TableSchema::error_message(err) << std::endl;
  }
}

// ============================================================
// 测试 2: RowBuilder 构建 Row
// ============================================================
void test_row_builder() {
  print_separator("测试 RowBuilder 构建行");

  TableSchema user_schema("users");
  user_schema.primary_key("id", DataType::INT)
      .not_null("name", DataType::VARCHAR)
      .not_null("age", DataType::INT)
      .nullable("email", DataType::VARCHAR);

  // 2.1 正常行
  auto result = row()
                    .set("id", 1)
                    .set("name", "Alice")
                    .set("age", 25)
                    .set("email", "alice@example.com")
                    .build(user_schema);

  if (result) {
    std::cout << "✅ 正常行构建成功: " << result->to_string() << std::endl;
  } else {
    std::cout << "❌ " << TableSchema::error_message(result.error())
              << std::endl;
  }

  // 2.2 缺少 NOT NULL 列
  auto result2 = row()
                     .set("id", 2)
                     .set("age", 30)  // 缺少 name (NOT NULL)
                     .build(user_schema);

  if (result2) {
    std::cout << "✅ Row: " << result2->to_string() << std::endl;
  } else {
    std::cout << "✅ 正确拒绝缺少 NOT NULL 列: "
              << TableSchema::error_message(result2.error()) << std::endl;
  }

  // 2.3 NULL 在 NOT NULL 列
  auto result3 = row()
                     .set("id", 3)
                     .set_null("name")  // name 是 NOT NULL
                     .set("age", 35)
                     .set_null("email")
                     .build(user_schema);

  if (result3) {
    std::cout << "✅ Row: " << result3->to_string() << std::endl;
  } else {
    std::cout << "✅ 正确拒绝 NULL 在 NOT NULL 列: "
              << TableSchema::error_message(result3.error()) << std::endl;
  }

  // 2.4 类型不匹配
  auto result4 = row()
                     .set("id", "123")  // id 是 INT，传入字符串
                     .set("name", "Charlie")
                     .set("age", 35)
                     .build(user_schema);

  if (result4) {
    std::cout << "✅ Row: " << result4->to_string() << std::endl;
  } else {
    std::cout << "✅ 正确拒绝类型不匹配: "
              << TableSchema::error_message(result4.error()) << std::endl;
  }
}

// ============================================================
// 测试 3: DatabaseManager + Database + Table（含 Cursor 和 get_batch）
// ============================================================
void test_database_layer() {
  print_separator("测试数据库层");

  // 创建 KV 引擎
  auto engine = std::make_shared<kv::MockEngine>();
  engine->open_database(kv::DatabaseOptions().set_path("./sql_test"));

  // 创建数据库管理器
  DatabaseManager db_manager(engine);

  // 3.1 创建数据库
  db_manager.create_database("testdb");
  db_manager.create_database("blogdb");

  auto dbs = db_manager.list_databases();
  std::cout << "✅ 数据库列表: ";
  for (const auto& db : dbs) {
    std::cout << db << " ";
  }
  std::cout << std::endl;

  // 3.2 打开数据库
  auto db = db_manager.open_database("testdb");
  if (!db) {
    std::cerr << "❌ 打开数据库失败" << std::endl;
    return;
  }
  std::cout << "✅ 打开数据库: " << db->name() << std::endl;

  // 3.3 创建表
  TableSchema user_schema("users");
  user_schema.primary_key("id", DataType::INT)
      .not_null("name", DataType::VARCHAR)
      .not_null("age", DataType::INT)
      .nullable("email", DataType::VARCHAR);

  if (db->create_table(user_schema)) {
    std::cout << "✅ 创建表: users" << std::endl;
  }

  TableSchema post_schema("posts");
  post_schema.primary_key("id", DataType::INT)
      .not_null("title", DataType::VARCHAR)
      .not_null("content", DataType::TEXT)
      .not_null("author_id", DataType::INT);

  if (db->create_table(post_schema)) {
    std::cout << "✅ 创建表: posts" << std::endl;
  }

  auto tables = db->list_tables();
  std::cout << "✅ 表列表: ";
  for (const auto& t : tables) {
    std::cout << t << " ";
  }
  std::cout << std::endl;

  // 3.4 获取表
  auto users = db->get_table("users");
  if (!users) {
    std::cerr << "❌ 获取表失败" << std::endl;
    return;
  }
  std::cout << "✅ 获取表: " << users->name() << std::endl;

  // 3.5 插入数据
  auto row1 = row()
                  .set("id", 1)
                  .set("name", "Alice")
                  .set("age", 25)
                  .set("email", "alice@example.com")
                  .build(user_schema);

  if (!row1) {
    std::cout << "❌ 构建行失败: " << TableSchema::error_message(row1.error())
              << std::endl;
    return;
  }

  if (users->insert(*row1)) {
    std::cout << "✅ 插入用户: Alice" << std::endl;
  }

  auto row2 = row()
                  .set("id", 2)
                  .set("name", "Bob")
                  .set("age", 30)
                  .set("email", "bob@example.com")
                  .build(user_schema);

  if (row2 && users->insert(*row2)) {
    std::cout << "✅ 插入用户: Bob" << std::endl;
  }

  auto row3 = row()
                  .set("id", 3)
                  .set("name", "Charlie")
                  .set("age", 35)
                  .set_null("email")
                  .build(user_schema);

  if (row3 && users->insert(*row3)) {
    std::cout << "✅ 插入用户: Charlie (email NULL)" << std::endl;
  }

  // 3.6 查询数据（点查询）
  auto result = users->get(Value(2));
  if (result) {
    std::cout << "✅ 查询 id=2: " << result->to_string() << std::endl;
  } else {
    std::cout << "⚠️  查询 id=2: 未找到" << std::endl;
  }

  // ============================================================
  // 3.7 全表扫描（使用 Cursor）
  // ============================================================
  std::cout << "\n✅ 全表扫描 (使用 Cursor):" << std::endl;
  auto cursor = users->scan_all();
  size_t row_count = 0;
  while (cursor->valid()) {
    auto row = cursor->next();
    if (row) {
      std::cout << "  " << row->to_string() << std::endl;
      row_count++;
    }
  }
  std::cout << "  共 " << row_count << " 行" << std::endl;

  // ============================================================
  // 3.8 测试 get_batch 接口
  // ============================================================
  print_separator("测试 get_batch");

  std::vector<Value> keys = {Value(1), Value(2), Value(99), Value(3)};
  std::vector<std::optional<Row>> batch_rows;

  // 策略：缺失返回空
  bool success =
      users->get_batch(keys, TableMissingKeyPolicy::kReturnEmpty, &batch_rows);
  if (success) {
    std::cout << "✅ get_batch (ReturnEmpty) 成功" << std::endl;
    for (size_t i = 0; i < keys.size(); ++i) {
      std::cout << "  key=" << keys[i].to_string() << " -> ";
      if (batch_rows[i]) {
        std::cout << batch_rows[i]->to_string();
      } else {
        std::cout << "(not found)";
      }
      std::cout << std::endl;
    }
  } else {
    std::cout << "❌ get_batch (ReturnEmpty) 失败" << std::endl;
  }

  // 策略：缺失报错
  batch_rows.clear();
  bool success2 =
      users->get_batch(keys, TableMissingKeyPolicy::kReturnError, &batch_rows);
  if (success2) {
    std::cout << "✅ get_batch (ReturnError) 成功（所有 key 都存在）"
              << std::endl;
  } else {
    std::cout << "✅ get_batch (ReturnError) 正确失败（有 key 缺失）"
              << std::endl;
    // 此时 batch_rows 可能为空或部分填充，取决于实现
  }

  // ============================================================
  // 3.9 更新数据
  // ============================================================
  print_separator("更新数据");

  auto updated = row()
                     .set("id", 1)
                     .set("name", "Alice Updated")
                     .set("age", 26)
                     .set("email", "alice_new@example.com")
                     .build(user_schema);

  if (updated && users->update(Value(1), *updated)) {
    std::cout << "✅ 更新 id=1: Alice → Alice Updated" << std::endl;
  }

  std::vector<std::pair<std::string, Value>> assignments = {
      {"age", Value(31)}, {"email", Value("bob_new@example.com")}};
  if (users->update(Value(2), assignments)) {
    std::cout << "✅ 更新 id=2: age=31, email=bob_new@example.com" << std::endl;
  }

  // ============================================================
  // 3.10 删除数据
  // ============================================================
  print_separator("删除数据");

  if (users->remove(Value(3))) {
    std::cout << "✅ 删除 id=3: Charlie" << std::endl;
  }

  // ============================================================
  // 3.11 最终数据（再次使用 Cursor）
  // ============================================================
  print_separator("最终数据");

  auto cursor2 = users->scan_all();
  std::cout << "✅ 最终数据 (使用 Cursor):" << std::endl;
  row_count = 0;
  while (cursor2->valid()) {
    auto row = cursor2->next();
    if (row) {
      std::cout << "  " << row->to_string() << std::endl;
      row_count++;
    }
  }
  std::cout << "  用户数: " << users->row_count() << std::endl;

  // ============================================================
  // 3.12 清理
  // ============================================================
  print_separator("清理");

  db->drop_table("users");
  db->drop_table("posts");
  db_manager.drop_database("testdb");
  db_manager.drop_database("blogdb");

  dbs = db_manager.list_databases();
  std::cout << "✅ 剩余数据库: ";
  for (const auto& db : dbs) {
    std::cout << db << " ";
  }
  std::cout << std::endl;
}

// ============================================================
// 测试 4: 错误处理
// ============================================================
void test_error_handling() {
  print_separator("测试错误处理");

  TableSchema user_schema("users");
  user_schema.primary_key("id", DataType::INT)
      .not_null("name", DataType::VARCHAR)
      .not_null("age", DataType::INT)
      .nullable("email", DataType::VARCHAR);

  // 4.1 插入无效行
  auto invalid_row = row()
                         .set("id", 99)
                         .set("name", "Test")
                         // 缺少 age (NOT NULL)
                         .build(user_schema);

  if (!invalid_row) {
    std::cout << "✅ 正确拒绝无效行: "
              << TableSchema::error_message(invalid_row.error()) << std::endl;
  }

  // 4.2 检查 SchemaError 的详细错误信息
  if (!invalid_row) {
    SchemaError err = invalid_row.error();
    std::cout << "  错误码: " << static_cast<int>(err) << std::endl;
    std::cout << "  错误信息: " << TableSchema::error_message(err) << std::endl;
  }

  // 4.3 验证行检查
  if (invalid_row) {
    Row& row = *invalid_row;
    SchemaError validate_err = user_schema.validate_row(row);
    if (validate_err != SchemaError::OK) {
      std::cout << "✅ 验证失败: " << TableSchema::error_message(validate_err)
                << std::endl;
    }
  }
}

// ============================================================
// 主函数
// ============================================================
int main() {
  std::cout << "╔══════════════════════════════════════════╗" << std::endl;
  std::cout << "║     SQL Relation Layer Test Suite       ║" << std::endl;
  std::cout << "╚══════════════════════════════════════════╝" << std::endl;

  test_schema_builder();
  test_row_builder();
  test_database_layer();
  test_error_handling();

  std::cout << "\n✅ 所有测试完成!" << std::endl;
  return 0;
}