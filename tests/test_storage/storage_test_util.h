// tests/test_storage/storage_test_util.h
//
// 存储层测试脚手架：一份用例跑两个引擎。
//
// 约定（见 storage/kv_engine/readme.md）：
//   - 一个进程一份 KVStore，N 条连接（connect()）；
//   - **凡是不涉及引擎特性的用例，都必须在 Mock 与 LevelDB 上各跑一遍** ——
//     两个引擎的语义必须一致（历史上已经踩过"只有 mock 被测到"的坑）。
#pragma once

#include <functional>
#include <memory>
#include <string>

#include "storage/kv_engine/kv_factory.h"

#if defined(SQLDB_HAVE_LEVELDB)
#include <filesystem>
#endif

namespace storagetest {

inline kv::DatabaseOptions mock_options(const std::string &name) {
  kv::DatabaseOptions options;
  options.set_path("mock://" + name);
  return options;
}

inline std::shared_ptr<kv::KVStore> open_mock(const std::string &name) {
  return kv::open_store(kv::EngineType::MOCK, mock_options(name));
}

// 每次用全新的目录：leveldb 的数据留在目录里，只 remove 文件名清不干净
inline std::string fresh_leveldb_path(const std::string &name) {
  static int counter = 0;
  return "/tmp/sqldb_storage_test_" + name + "_" + std::to_string(counter++);
}

#if defined(SQLDB_HAVE_LEVELDB)
inline std::shared_ptr<kv::KVStore> open_leveldb(const std::string &name) {
  const std::string path = fresh_leveldb_path(name);
  std::filesystem::remove_all(path);
  kv::DatabaseOptions options;
  options.set_path(path).set_create_if_missing(true).set_error_if_exists(false);
  return kv::open_store(kv::EngineType::LEVELDB, options);
}

// 打开一个指定目录的 leveldb（测"重开同一个库"用）
inline std::shared_ptr<kv::KVStore> open_leveldb_at(const std::string &path) {
  kv::DatabaseOptions options;
  options.set_path(path).set_create_if_missing(true).set_error_if_exists(false);
  return kv::open_store(kv::EngineType::LEVELDB, options);
}
#endif

// 同一份用例在 Mock 与 LevelDB 上各跑一遍。
// body 拿到的是**存储**（要几条连接自己 connect()）。
inline void run_on_both(const std::string &name,
                        const std::function<void(kv::KVStore &)> &body) {
  auto mock = open_mock(name);
  CHECK(mock != nullptr);
  if (mock != nullptr) {
    body(*mock);
  }
#if defined(SQLDB_HAVE_LEVELDB)
  auto leveldb = open_leveldb(name);
  CHECK(leveldb != nullptr);
  if (leveldb != nullptr) {
    body(*leveldb);
    leveldb->close();
  }
#endif
}

// 只在一个引擎上跑（引擎特有的行为：故障注入、持久化……）
inline void run_on_mock(const std::string &name,
                        const std::function<void(kv::KVStore &)> &body) {
  auto mock = open_mock(name);
  CHECK(mock != nullptr);
  if (mock != nullptr) {
    body(*mock);
  }
}

inline std::string value_of(kv::KVEngine &conn, const kv::Key &key) {
  kv::ByteValue value;
  if (conn.get(key, &value) != kv::Status::OK) {
    return "<missing>";
  }
  return value;
}

inline std::vector<kv::Key> scan_keys(kv::KVEngine &conn,
                                      const kv::KeyRange &range = {}) {
  std::vector<kv::Key> keys;
  auto it = conn.new_iterator(range);
  if (it == nullptr) {
    return keys;
  }
  for (it->seek_to_first(); it->valid(); it->next()) {
    keys.push_back(it->key());
  }
  return keys;
}

} // namespace storagetest
