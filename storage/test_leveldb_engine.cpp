// 使用 LevelDB 引擎
#include "storage_engine.h"
#include "leveldb_engine.h"

int main(){
  storage::StorageOptions options;
  options.type = storage::StorageType::LEVELDB;
  options.path = "./my_db";

  auto engine = storage::StorageEngineFactory::create_engine(options);

  // 创建表
  storage::TableSchema schema;
  schema.table_name = "users";
  schema.columns = {{"id", storage::DataType::INTEGER},
                    {"name", storage::DataType::STRING},
                    {"age", storage::DataType::INTEGER}};
  schema.primary_key_index = 0;
  engine->create_table(schema);

  // 插入数据
  storage::Row row;
  row.push_back(storage::Value(1));
  row.push_back(storage::Value("Alice"));
  row.push_back(storage::Value(30));
  engine->insert("users", row);

  // 查询
  storage::Row result;
  engine->get_by_key("users", "1", result);

  // 范围查询
  storage::KeyRange range("data:users:1", "data:users:10");
  auto rows = engine->get_by_key_range("users", range);
}
