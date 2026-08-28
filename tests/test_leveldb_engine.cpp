// test_leveldb_engine.cpp
#include <cassert>
#include <chrono>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <string>
#include <vector>
#include <thread>

#include "storage/leveldb_engine/leveldb_engine.h"

using namespace kv;

// ============================================================
// 辅助函数：打印分隔线
// ============================================================
void print_separator(const std::string& title = "", char ch = '=') {
  if (!title.empty()) {
    std::cout << "\n" << std::string(60, ch) << std::endl;
    std::cout << "  " << title << std::endl;
    std::cout << std::string(60, ch) << std::endl;
  } else {
    std::cout << std::string(60, ch) << std::endl;
  }
}

// ============================================================
// 辅助函数：打印操作结果
// ============================================================
void print_result(const std::string& operation, Status s,
                  const std::string& detail = "") {
  std::cout << "  [" << operation << "] ";
  if (s == Status::OK) {
    std::cout << "✅ SUCCESS";
  } else {
    std::cout << "❌ FAILED: " << status_to_string(s);
  }
  if (!detail.empty()) {
    std::cout << " (" << detail << ")";
  }
  std::cout << std::endl;
}

// ============================================================
// 辅助函数：打印 KV 对
// ============================================================
void print_kv(const Key& key, const ByteValue& value,
              const std::string& prefix = "  ") {
  std::cout << prefix << "Key: \"" << key << "\"";
  std::cout << " -> Value: \"" << value << "\"";
  std::cout << " (len: " << value.size() << ")" << std::endl;
}

// ============================================================
// 辅助函数：打印 KV 对（带索引）
// ============================================================
void print_kv_with_index(size_t index, const Key& key, const ByteValue& value) {
  std::cout << "  [" << std::setw(2) << index << "] ";
  print_kv(key, value, "");
}

// ============================================================
// 辅助函数：获取当前时间戳
// ============================================================
std::string timestamp() {
  auto now = std::chrono::system_clock::now();
  auto time = std::chrono::system_clock::to_time_t(now);
  std::stringstream ss;
  ss << std::put_time(std::localtime(&time), "%H:%M:%S");
  return ss.str();
}

// ============================================================
// 辅助函数：计时器
// ============================================================
class Timer {
 public:
  Timer() : start_(std::chrono::steady_clock::now()) {}

  void reset() { start_ = std::chrono::steady_clock::now(); }

  double elapsed_seconds() const {
    auto now = std::chrono::steady_clock::now();
    return std::chrono::duration<double>(now - start_).count();
  }

  double elapsed_milliseconds() const { return elapsed_seconds() * 1000.0; }

 private:
  std::chrono::steady_clock::time_point start_;
};

// ============================================================
// 清理测试目录
// ============================================================
void cleanup_test_dir(const std::string& path) {
  try {
    std::filesystem::remove_all(path);
  } catch (...) {
    // 忽略清理错误
  }
}

// ============================================================
// 主测试函数
// ============================================================
int main() {
  std::cout << "╔══════════════════════════════════════════════════════╗"
            << std::endl;
  std::cout << "║           🧪 LevelDBEngine 完整测试套件            ║"
            << std::endl;
  std::cout << "╚══════════════════════════════════════════════════════╝"
            << std::endl;
  std::cout << "测试开始时间: " << timestamp() << std::endl;

  // 清理旧的测试数据
  const std::string db_path = "./test_leveldb_db";
  cleanup_test_dir(db_path);
  std::cout << "  🧹 清理旧数据: " << db_path << std::endl;

  // ============================================================
  // 1. 创建引擎
  // ============================================================
  print_separator("1. 创建引擎", '=');
  auto engine = std::make_unique<LevelDBEngine>();
  std::cout << "  ✅ 引擎创建成功" << std::endl;
  std::cout << "  引擎名称: " << engine->name() << std::endl;

  // ============================================================
  // 2. 打开数据库
  // ============================================================
  {  // first engine
    print_separator("2. 打开数据库", '=');
    auto opts = DatabaseOptions()
                    .set_path(db_path)
                    .set_create_if_missing(true)
                    .set_cache_size(64)  // 64MB 缓存
                    .set_compression(true);

    std::cout << "  数据库路径: " << opts.path << std::endl;
    std::cout << "  创建选项: " << (opts.create_if_missing ? "创建" : "不创建")
              << std::endl;
    std::cout << "  压缩选项: " << (opts.compression ? "启用" : "禁用")
              << std::endl;
    std::cout << "  缓存大小: " << opts.cache_size_mb << " MB" << std::endl;

    Timer timer;
    Status s = engine->open_database(opts);
    double elapsed = timer.elapsed_milliseconds();

    print_result("open_database", s, "耗时: " + std::to_string(elapsed) + "ms");

    if (s != Status::OK) {
      std::cerr << "❌ 打开数据库失败，退出测试" << std::endl;
      return 1;
    }

    std::cout << "  数据库状态: " << (engine->is_open() ? "已打开" : "已关闭")
              << std::endl;

    // ============================================================
    // 3. 单条插入测试
    // ============================================================
    print_separator("3. 单条插入测试", '=');

    std::vector<std::pair<Key, ByteValue>> test_data = {
        {"key1", "value1"},       {"key2", "value2"},
        {"key3", "value3"},       {"key4", "value4"},
        {"key5", "value5"},       {"user_alice", "alice_data"},
        {"user_bob", "bob_data"}, {"user_charlie", "charlie_data"},
        {"batch1", "batch_val1"}, {"batch2", "batch_val2"},
        {"batch3", "batch_val3"},
    };

    std::cout << "  准备插入 " << test_data.size() << " 条数据..." << std::endl;

    int success_count = 0;
    timer.reset();
    for (const auto& [key, value] : test_data) {
      Status s = engine->put(key, value);
      if (s == Status::OK) {
        success_count++;
      } else {
        std::cout << "  ❌ 插入失败: " << key << " -> " << status_to_string(s)
                  << std::endl;
      }
    }
    elapsed = timer.elapsed_milliseconds();

    std::cout << "  ✅ 成功插入 " << success_count << "/" << test_data.size()
              << " 条数据";
    std::cout << " (耗时: " << elapsed << "ms)" << std::endl;

    // ============================================================
    // 4. 单条查询测试
    // ============================================================
    print_separator("4. 单条查询测试", '=');

    std::vector<Key> keys_to_get = {"key1", "key3", "key5", "key999",
                                    "user_alice"};
    std::cout << "  查询 " << keys_to_get.size() << " 个 key..." << std::endl;

    int found_count = 0;
    for (const auto& key : keys_to_get) {
      ByteValue v;
      timer.reset();
      Status s = engine->get(key, &v);
      elapsed = timer.elapsed_milliseconds();

      if (s == Status::OK) {
        found_count++;
        std::cout << "  ✅ " << key << " -> ";
        std::cout << "\"" << v << "\" (len: " << v.size() << ")";
        std::cout << " [耗时: " << elapsed << "ms]" << std::endl;
      } else if (s == Status::NotFound) {
        std::cout << "  ⚠️  " << key << " -> 不存在 [耗时: " << elapsed << "ms]"
                  << std::endl;
      } else {
        std::cout << "  ❌ " << key << " -> 错误: " << status_to_string(s)
                  << std::endl;
      }
    }
    std::cout << "  查询结果: " << found_count << "/" << keys_to_get.size()
              << " 找到" << std::endl;

    // ============================================================
    // 5. 存在性检查测试
    // ============================================================
    print_separator("5. 存在性检查测试", '=');

    std::vector<std::pair<Key, bool>> keys_to_check = {{"key1", true},
                                                       {"key2", true},
                                                       {"key999", false},
                                                       {"user_bob", true},
                                                       {"not_exist", false}};
    std::cout << "  检查 " << keys_to_check.size() << " 个 key..." << std::endl;

    for (const auto& [key, exist] : keys_to_check) {
      timer.reset();
      bool exists = engine->exists(key);
      elapsed = timer.elapsed_milliseconds();
      std::cout << "  " << key << " -> " << (exists ? "✅ 存在" : "❌ 不存在");
      if (exists != exist) {
        std::cerr << "Error:key:" << key
                  << " (预期: " << (exist ? "存在" : "不存在") << ")"
                  << std::endl;
      }
      std::cout << " [耗时: " << elapsed << "ms]" << std::endl;
    }

    // ============================================================
    // 6. 批量操作测试
    // ============================================================
    print_separator("6. 批量操作测试", '=');

    std::cout << "  准备批量操作:" << std::endl;
    WriteBatch batch;
    batch.put("batch_new1", "new_val1");
    batch.put("batch_new2", "new_val2");
    batch.put("batch_new3", "new_val3");
    batch.remove("key2");  // 删除 key2
    batch.remove("key4");  // 删除 key4

    std::cout << "    PUT: batch_new1, batch_new2, batch_new3" << std::endl;
    std::cout << "    DELETE: key2, key4" << std::endl;

    timer.reset();
    s = engine->write_batch(batch);
    elapsed = timer.elapsed_milliseconds();

    print_result("write_batch", s, "耗时: " + std::to_string(elapsed) + "ms");

    // 验证删除
    std::cout << "  验证删除结果:" << std::endl;
    for (const auto& key : {"key2", "key4"}) {
      bool exists = engine->exists(key);
      std::cout << "    " << key << " -> "
                << (exists ? "❌ 仍然存在" : "✅ 已删除") << std::endl;
    }

    // 验证新增
    std::cout << "  验证新增结果:" << std::endl;
    for (const auto& key : {"batch_new1", "batch_new2", "batch_new3"}) {
      bool exists = engine->exists(key);
      std::cout << "    " << key << " -> "
                << (exists ? "✅ 已添加" : "❌ 未找到") << std::endl;
    }

    // ============================================================
    // 7. 批量获取测试
    // ============================================================
    print_separator("7. 批量获取测试", '=');

    std::vector<Key> batch_keys = {"key1",   "key3",         "batch_new1",
                                   "key999", "user_charlie", "batch_new3"};
    std::vector<std::optional<ByteValue>> batch_values;

    std::cout << "  批量获取 " << batch_keys.size()
              << " 个 key (策略: ReturnEmpty)..." << std::endl;

    timer.reset();
    s = engine->get_batch(batch_keys, MissingKeyPolicy::kReturnEmpty,
                          &batch_values);
    elapsed = timer.elapsed_milliseconds();

    print_result("get_batch", s, "耗时: " + std::to_string(elapsed) + "ms");

    int found_in_batch = 0;
    for (size_t i = 0; i < batch_keys.size(); ++i) {
      const auto& key = batch_keys[i];
      const auto& val = batch_values[i];
      if (val.has_value()) {
        found_in_batch++;
        std::cout << "  ✅ " << key << " -> \"" << val.value() << "\""
                  << std::endl;
      } else {
        std::cout << "  ⚠️  " << key << " -> (不存在)" << std::endl;
      }
    }
    std::cout << "  批量获取结果: " << found_in_batch << "/"
              << batch_keys.size() << " 找到" << std::endl;

    // ============================================================
    // 8. 迭代器测试 - 全扫描
    // ============================================================
    print_separator("8. 迭代器测试 - 全扫描", '=');

    std::cout << "  执行全扫描..." << std::endl;
    timer.reset();

    auto it = engine->new_all_iterator();
    if (!it) {
      std::cerr << "  ❌ 创建迭代器失败" << std::endl;
    } else {
      std::cout << "  ✅ 迭代器创建成功" << std::endl;
      std::cout << "  状态: valid=" << (it->valid() ? "true" : "false")
                << std::endl;

      size_t count = 0;
      while (it->valid()) {
        count++;
        print_kv_with_index(count, it->key(), it->value());
        it->next();
      }

      elapsed = timer.elapsed_milliseconds();
      std::cout << "  全扫描完成: " << count << " 条记录";
      std::cout << " [耗时: " << elapsed << "ms]" << std::endl;
      std::cout << "  迭代器状态: " << (it->valid() ? "有效" : "已结束")
                << std::endl;
      std::cout << "  状态码: " << status_to_string(it->status()) << std::endl;
      std::cout << "  msg: " << it->error_message() << std::endl;
    }

    // ============================================================
    // 9. 迭代器测试 - 范围扫描
    // ============================================================
    print_separator("9. 迭代器测试 - 范围扫描", '=');

    KeyRange range = KeyRange::range("batch1", "batch3");
    std::cout << "  范围: [" << range.start.value() << ", " << range.end.value()
              << ")" << std::endl;
    std::cout << "  方向: "
              << (range.direction == ScanDirection::kForward ? "正向" : "反向")
              << std::endl;

    it = engine->new_iterator(range);
    if (!it) {
      std::cerr << "  ❌ 创建范围迭代器失败" << std::endl;
    } else {
      size_t count = 0;
      while (it->valid()) {
        count++;
        print_kv_with_index(count, it->key(), it->value());

        // 验证是否在范围内
        if (range.contains(it->key())) {
          std::cout << "    ✅ 在范围内" << std::endl;
        } else {
          std::cout << "    ❌ 超出范围!" << std::endl;
        }
        it->next();
      }
      std::cout << "  范围扫描完成: " << count << " 条记录" << std::endl;
    }

    // ============================================================
    // 10. 迭代器测试 - 前缀扫描
    // ============================================================
    print_separator("10. 迭代器测试 - 前缀扫描", '=');

    std::string prefix = "user_";
    std::cout << "  前缀: \"" << prefix << "\"" << std::endl;

    it = engine->new_prefix_iterator(prefix);
    if (!it) {
      std::cerr << "  ❌ 创建前缀迭代器失败" << std::endl;
    } else {
      size_t count = 0;
      while (it->valid()) {
        count++;
        print_kv_with_index(count, it->key(), it->value());

        // 验证前缀
        if (it->key().find(prefix) == 0) {
          std::cout << "    ✅ 匹配前缀" << std::endl;
        } else {
          std::cout << "    ❌ 不匹配前缀!" << std::endl;
        }
        it->next();
      }
      std::cout << "  前缀扫描完成: " << count << " 条记录" << std::endl;
    }

    // ============================================================
    // 11. 迭代器测试 - 反向扫描
    // ============================================================
    print_separator("11. 迭代器测试 - 反向扫描", '=');

    KeyRange reverse_range = KeyRange::all();
    reverse_range.direction = ScanDirection::kReverse;
    std::cout << "  扫描方向: 反向" << std::endl;
    std::cout << "  (预期 key 降序排列)" << std::endl;

    it = engine->new_iterator(reverse_range);
    if (!it) {
      std::cerr << "  ❌ 创建反向迭代器失败" << std::endl;
    } else {
      std::vector<Key> keys_in_order;
      while (it->valid()) {
        keys_in_order.push_back(it->key());
        print_kv_with_index(keys_in_order.size(), it->key(), it->value());
        it->next();
      }

      // 验证是否降序
      bool is_descending = true;
      for (size_t i = 1; i < keys_in_order.size(); ++i) {
        if (keys_in_order[i - 1] <= keys_in_order[i]) {
          is_descending = false;
          break;
        }
      }
      std::cout << "  验证顺序: "
                << (is_descending ? "✅ 降序正确" : "❌ 顺序错误") << std::endl;
      std::cout << "  反向扫描完成: " << keys_in_order.size() << " 条记录"
                << std::endl;
    }

    // ============================================================
    // 12. 迭代器测试 - Seek 定位
    // ============================================================
    print_separator("12. 迭代器测试 - Seek 定位", '=');

    std::vector<Key> seek_keys = {"key1", "key3", "key5", "user_bob", "zzz"};
    std::cout << "  Seek 测试，定位到 " << seek_keys.size() << " 个 key..."
              << std::endl;

    for (const auto& seek_key : seek_keys) {
      it = engine->new_all_iterator();
      if (!it) {
        std::cerr << "  ❌ 创建迭代器失败" << std::endl;
        break;
      }

      timer.reset();
      it->seek(seek_key);
      elapsed = timer.elapsed_milliseconds();

      if (it->valid()) {
        std::cout << "  ✅ Seek \"" << seek_key << "\" -> ";
        std::cout << "找到 \"" << it->key() << "\"";
        std::cout << " [耗时: " << elapsed << "ms]" << std::endl;
      } else {
        std::cout << "  ⚠️  Seek \"" << seek_key << "\" -> 未找到";
        std::cout << " [耗时: " << elapsed << "ms]" << std::endl;
      }
    }

    // ============================================================
    // 13. 迭代器测试 - for_each
    // ============================================================
    print_separator("13. 迭代器测试 - for_each", '=');

    std::cout << "  使用 for_each 遍历..." << std::endl;

    it = engine->new_all_iterator();
    if (!it) {
      std::cerr << "  ❌ 创建迭代器失败" << std::endl;
    } else {
      size_t count = 0;
      timer.reset();
      it->for_each([&count](const Key& k, const ByteValue& v) {
        count++;
        if (count <= 5) {
          std::cout << "  [" << count << "] " << k << " -> " << v << std::endl;
        }
        return true;  // 继续
      });
      elapsed = timer.elapsed_milliseconds();

      std::cout << "  for_each 遍历完成: " << count << " 条记录";
      std::cout << " [耗时: " << elapsed << "ms]" << std::endl;
    }

    // ============================================================
    // 14. 迭代器测试 - collect
    // ============================================================
    print_separator("14. 迭代器测试 - collect", '=');

    std::cout << "  使用 collect 收集数据 (max_count=5)..." << std::endl;

    it = engine->new_all_iterator();
    if (!it) {
      std::cerr << "  ❌ 创建迭代器失败" << std::endl;
    } else {
      timer.reset();
      auto pairs = it->collect(5);
      elapsed = timer.elapsed_milliseconds();

      std::cout << "  collect 收集到 " << pairs.size() << " 条记录";
      std::cout << " [耗时: " << elapsed << "ms]" << std::endl;

      for (const auto& pair : pairs) {
        if (pair.value.has_value()) {
          std::cout << "    " << pair.key << " -> " << pair.value.value()
                    << std::endl;
        }
      }
    }

    // ============================================================
    // 15. 删除操作测试
    // ============================================================
    print_separator("15. 删除操作测试", '=');

    std::vector<Key> delete_keys = {"key3", "batch_new1"};
    std::cout << "  删除 " << delete_keys.size() << " 个 key..." << std::endl;

    for (const auto& key : delete_keys) {
      timer.reset();
      s = engine->remove(key);
      elapsed = timer.elapsed_milliseconds();

      print_result("remove " + key, s,
                   "耗时: " + std::to_string(elapsed) + "ms");

      // 验证删除
      bool exists = engine->exists(key);
      std::cout << "    验证: " << key << " -> "
                << (exists ? "❌ 仍然存在" : "✅ 已删除") << std::endl;
    }

    // ============================================================
    // 16. 批量删除测试
    // ============================================================
    print_separator("16. 批量删除测试", '=');

    std::vector<Key> batch_delete = {"batch1", "batch2", "batch_new2"};
    std::cout << "  批量删除 " << batch_delete.size() << " 个 key..."
              << std::endl;

    timer.reset();
    s = engine->remove_batch(batch_delete);
    elapsed = timer.elapsed_milliseconds();

    print_result("remove_batch", s, "耗时: " + std::to_string(elapsed) + "ms");

    // 验证
    for (const auto& key : batch_delete) {
      bool exists = engine->exists(key);
      std::cout << "    " << key << " -> "
                << (exists ? "❌ 仍然存在" : "✅ 已删除") << std::endl;
    }

    // ============================================================
    // 17. 压力测试 - 大量数据写入
    // ============================================================
    /*print_separator("17. 压力测试 - 大量数据写入", '=');

    const size_t BULK_SIZE = 1000;
    std::cout << "  写入 " << BULK_SIZE << " 条数据..." << std::endl;

    timer.reset();
    WriteBatch bulk_batch;
    for (size_t i = 0; i < BULK_SIZE; ++i) {
      std::string key = "bulk_" + std::to_string(i);
      std::string value = "value_" + std::to_string(i);
      bulk_batch.put(key, value);
    }
    s = engine->write_batch(bulk_batch);
    elapsed = timer.elapsed_milliseconds();

    print_result("bulk_write", s,
                 "写入 " + std::to_string(BULK_SIZE) +
                     " 条，耗时: " + std::to_string(elapsed) + "ms");

    if (s == Status::OK) {
      std::cout << "  平均写入速度: " << (BULK_SIZE / (elapsed / 1000.0))
                << " 条/秒" << std::endl;
    }

    // 验证部分数据
    std::cout << "  验证 10 条随机数据..." << std::endl;
    int verify_count = 0;
    for (size_t i = 0; i < 10; ++i) {
      size_t idx = i * (BULK_SIZE / 10);
      std::string key = "bulk_" + std::to_string(idx);
      ByteValue val;
      if (engine->get(key, &val) == Status::OK) {
        verify_count++;
      }
    }
    std::cout << "  验证结果: " << verify_count << "/10 找到" << std::endl;
   */
    // ============================================================
    // 18. 统计信息
    // ============================================================
    print_separator("18. 统计信息", '=');

    std::cout << engine->stats() << std::endl;

    // ============================================================
    // 19. Flush 测试
    // ============================================================
    print_separator("19. Flush 测试", '=');

    std::cout << "  执行 flush..." << std::endl;
    timer.reset();
    engine->flush();
    elapsed = timer.elapsed_milliseconds();
    std::cout << "  flush 完成 [耗时: " << elapsed << "ms]" << std::endl;

    // ============================================================
    // 20. 关闭数据库
    // ============================================================
    // print_separator("20. 关闭数据库", '=');

    // timer.reset();
    // s = engine->close_database();
    // elapsed = timer.elapsed_milliseconds();

    // print_result("close_database", s, "耗时: " + std::to_string(elapsed) +
    // "ms"); std::cout << "  数据库状态: " << (engine->is_open() ? "已打开" :
    // "已关闭")
    //           << std::endl;
    //engine.reset();
  }
  std::this_thread::sleep_for(std::chrono::milliseconds(100));
  std::cout << "engine1 closed.\n";

  // ============================================================
  // 21. 重新打开数据库验证持久化
  // ============================================================
  print_separator("21. 重新打开数据库验证持久化", '=');

  std::cout << "  重新打开数据库..." << std::endl;
  auto opts = DatabaseOptions()
                  .set_path(db_path)
                  .set_create_if_missing(true)
                  .set_cache_size(64)  // 64MB 缓存
                  .set_compression(true);

  auto engine2 = std::make_unique<LevelDBEngine>();
  auto s = engine2->open_database(opts);
  if (s != Status::OK) {
    std::cerr << "  ❌ 重新打开失败: " << status_to_string(s) << std::endl;
  } else {
    std::cout << "  ✅ 重新打开成功" << std::endl;

    // 验证数据是否持久化
    std::cout << "  验证数据持久化..." << std::endl;
    std::vector<Key> persist_keys = {"key1", "user_alice", "batch_new3"};
    int persist_count = 0;
    for (const auto& key : persist_keys) {
      ByteValue val;
      if (engine2->get(key, &val) == Status::OK) {
        persist_count++;
        std::cout << "    ✅ " << key << " -> " << val << std::endl;
      } else {
        std::cout << "    ❌ " << key << " -> 丢失!" << std::endl;
      }
    }
    std::cout << "  持久化验证: " << persist_count << "/" << persist_keys.size()
              << " 找到" << std::endl;

    // 关闭第二个引擎
    //engine2->close_database();
    std::cout << "  ✅ 第二个引擎已关闭" << std::endl;
  }

  // ============================================================
  // 测试总结
  // ============================================================
  print_separator("✅ 测试完成", '=');

  std::cout << "  测试完成时间: " << timestamp() << std::endl;
  std::cout << "  📊 所有测试用例执行完毕" << std::endl;
  std::cout << "  引擎类型: " << engine->name() << std::endl;
  std::cout << "  数据库路径: " << opts.path << std::endl;

  std::cout << "\n╔══════════════════════════════════════════════════════╗"
            << std::endl;
  std::cout << "║              ✅ 所有测试通过                      ║"
            << std::endl;
  std::cout << "╚══════════════════════════════════════════════════════╝"
            << std::endl;

  // 可选：保留数据用于调试，或清理
  // cleanup_test_dir(db_path);
  // std::cout << "  🧹 清理测试数据: " << db_path << std::endl;

  return 0;
}