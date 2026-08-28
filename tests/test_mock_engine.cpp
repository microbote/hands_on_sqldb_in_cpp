// test_mock_engine.cpp
#include <iostream>

#include "storage/mock_engine/mock_engine.h"

using namespace kv;

int main() {
  std::cout << "🧪 MockEngine 测试" << std::endl;
  std::cout << std::string(50, '=') << std::endl;

  // 创建引擎
  auto engine = std::make_unique<MockEngine>();

  // 打开数据库
  auto opts = DatabaseOptions()
    .set_path("./mock_db")
    .set_create_if_missing(true);

  Status s = engine->open_database(opts);
  if (s != Status::OK) {
    std::cerr << "打开失败: " << status_to_string(s) << std::endl;
    return 1;
  }
  std::cout << "✅ 数据库已打开" << std::endl;

  // 1. 基本操作
  std::cout << "\n=== 基本操作 ===" << std::endl;
  s = engine->put("key1", "value1");
  if(s != Status::OK) {
    std::cerr << "插入失败: key1" << status_to_string(s) << std::endl;
  }
  else{
    std::cout << "✅ 插入成功: key1" << std::endl;
  }
  s = engine->put("key2", "value2");
  if(s != Status::OK) {
    std::cerr << "插入失败: key2" << status_to_string(s) << std::endl;
  }
  else{
    std::cout << "✅ 插入成功: key2" << std::endl;
  }

  ByteValue v;
  if (engine->get("key1", &v) == Status::OK) {
    std::cout << "Succ:key1 = " << v << std::endl;
  }
  else{
    std::cerr << "Error:key1 不存在" << std::endl;
  }

  // 2. 批量操作
  std::cout << "\n=== 批量操作 ===" << std::endl;
  WriteBatch batch;
  batch.put("batch1", "val1");
  batch.put("batch2", "val2");
  batch.remove("key2");
  s = engine->write_batch(batch);
  if (s != Status::OK) {
    std::cerr << "批量操作失败: " << status_to_string(s) << std::endl;
  }
  else{
    std::cout << "✅ 批量操作成功" << std::endl;
  }

  // 3. 迭代器
  std::cout << "\n=== 迭代器扫描 ===" << std::endl;
  auto it = engine->new_all_iterator();
  int count = 0;
  while (it->valid()) {
    count++;
    std::cout << it->key() << " -> " << it->value() << std::endl;
    it->next();
  }
  if(count != 3) {
    std::cerr << "迭代器扫描失败, real:3, cnt:"<< count << std::endl;
  }

  // 4. 范围扫描
  std::cout << "\n=== 范围扫描 [batch1, batch3) ===" << std::endl;
  auto range = KeyRange::range("batch1", "batch3");
  it = engine->new_iterator(range);

  while (it->valid()) {
    std::cout << it->key() << " -> " << it->value() << std::endl;

    if(it->key()>= "batch3"){
      std::cerr << "范围扫描失败, key["<<it->key()<<" over range:batch3" << std::endl;
    }
    it->next();
  }

  // 5. 前缀扫描
  std::cout << "\n=== 前缀扫描 'batch' ===" << std::endl;
  it = engine->new_prefix_iterator("batch");
  count = 0;
  it->for_each([&count](const Key& k, const ByteValue& v) {
    std::cout << k << " -> " << v << std::endl;
    count++;
    //判断前缀
    if(!k.starts_with("batch")) {
      std::cerr << "前缀扫描失败, key["<<k<<"] not start with 'batch'" << std::endl;
    }
    return true;
  });
  std::cout << "prefixscan of [batch], real:2, count: " << count << std::endl;

  // 6. 打印统计信息
  std::cout << "\n" << engine->stats() << std::endl;

  // 7. 关闭
  engine->close_database();
  std::cout << "✅ 数据库已关闭" << std::endl;

  return 0;
}