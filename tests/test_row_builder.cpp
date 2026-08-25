// test_builder.cpp
#include "mock_engine.h"
#include "row_builder.h"
#include <iostream>

int main() {
  auto engine = storage::StorageEngineFactory::create_engine(
      storage::StorageOptions{storage::StorageType::MOCK});

  // 1. 创建表
  storage::TableSchema schema;
  schema.table_name = "users";
  schema.columns = {{"id", storage::DataType::INTEGER},
                    {"name", storage::DataType::STRING},
                    {"age", storage::DataType::INTEGER},
                    {"email", storage::DataType::STRING}};
  schema.primary_key_index = 0;
  engine->create_table(schema);

  // 2. 插入数据 - 使用 RowBuilder
  auto row1 = storage::row()
                  .set("id", 1)
                  .set("name", "Alice")
                  .set("age", 30)
                  .set("email", "alice@example.com")
                  .build(schema);
  engine->insert("users", row1);

  // 3. 插入数据 - 可读性更强
  auto row2 = storage::row()
                  .set("id", 2)
                  .set("name", "Bob")
                  .set("age", 25)
                  .set_null("email")  // 显式 NULL
                  .build(schema);
  engine->insert("users", row2);

  // 4. 使用 make_row（更简洁）
  auto row3 = storage::make_row(
      schema, {{"id", storage::Value(3)},
               {"name", storage::Value("Charlie")},
               {"age", storage::Value(35)},
               {"email", storage::Value("charlie@example.com")}});
  engine->insert("users", row3);

  // 5. 更新 - 使用 UpdateBuilder
  auto assignments = storage::update()
                         .set("age", 31)
                         .set("email", "alice_new@example.com")
                         .build();
  engine->update_by_key("users", "1", assignments);

  // 6. 查询并打印
  storage::Row result;
  if (engine->get_by_key("users", "1", result)) {
    std::cout << "User 1: ";
    for (const auto& val : result) {
      std::cout << val.to_string() << " ";
    }
    std::cout << std::endl;
  }

  // 7. 批量插入
  std::vector<storage::Row> batch = {
      storage::row()
          .set("id", 4)
          .set("name", "David")
          .set("age", 28)
          .build(schema),
      storage::row()
          .set("id", 5)
          .set("name", "Eve")
          .set("age", 32)
          .build(schema),
      storage::row()
          .set("id", 6)
          .set("name", "Frank")
          .set("age", 29)
          .build(schema),
  };
  engine->insert_batch("users", batch);

  // 8. 打印所有数据
  auto all_rows = engine->scan_all("users");
  for (const auto& row : all_rows) {
    for (const auto& val : row) {
      std::cout << val.to_string() << " ";
    }
    std::cout << std::endl;
  }

  return 0;
}