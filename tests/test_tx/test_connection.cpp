// tests/test_tx/test_connection.cpp
//
// 多连接：一个进程一份存储（KVStore），每个 session 一条连接（KVEngine）。
//
// 语义（见 storage/kv_engine/kv_engine.h）：
//   - 连接各自持有事务缓冲：没提交的写别的连接看不见；
//   - 写槽在 Store 上（悲观单写者）：同时只有一条连接能写；
//   - 读者不阻塞：读的是**已提交**状态，长扫描也不会被提交撕裂
//     （LevelDB 迭代器创建即钉住版本；Mock 迭代器创建时物化）。
//
// 每个用例都在 Mock 与 LevelDB 上各跑一遍：两个引擎必须语义一致。
#include "test_framework.h"

#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "storage/kv_engine/kv_factory.h"

#if defined(SQLDB_HAVE_LEVELDB)
#include <cstdio>
#include <filesystem>
#endif

namespace {

void run_on_mock(const std::function<void(kv::KVStore &)> &body) {
  kv::DatabaseOptions options;
  options.set_path("mock://connection-test");
  auto store = kv::open_store(kv::EngineType::MOCK, options);
  CHECK(store != nullptr);
  if (store != nullptr) {
    body(*store);
  }
}

#if defined(SQLDB_HAVE_LEVELDB)
void run_on_leveldb(const std::function<void(kv::KVStore &)> &body) {
  // 每次用一个全新的目录：leveldb 的旧数据会留在目录里，不能只 remove 文件名
  static int counter = 0;
  const std::string path =
      std::string("/tmp/sqldb_connection_test_") + std::to_string(counter++);
  std::filesystem::remove_all(path);
  kv::DatabaseOptions options;
  options.set_path(path).set_create_if_missing(true).set_error_if_exists(false);
  auto store = kv::open_store(kv::EngineType::LEVELDB, options);
  CHECK(store != nullptr);
  if (store != nullptr) {
    body(*store);
    store->close();
  }
}
#endif

// 同一份用例在两个引擎上各跑一遍
void run_on_both(const std::function<void(kv::KVStore &)> &body) {
  run_on_mock(body);
#if defined(SQLDB_HAVE_LEVELDB)
  run_on_leveldb(body);
#endif
}

std::string read(kv::KVEngine &conn, const kv::Key &key) {
  kv::ByteValue value;
  if (conn.get(key, &value) != kv::Status::OK) {
    return "<missing>";
  }
  return value;
}

std::vector<std::string> scan_keys(kv::KVEngine &conn) {
  std::vector<std::string> keys;
  auto it = conn.new_iterator(kv::KeyRange::all());
  if (it == nullptr) {
    return keys;
  }
  for (it->seek_to_first(); it->valid(); it->next()) {
    keys.push_back(it->key());
  }
  return keys;
}

} // namespace

TEST(Connections, TransactionsArePerConnectionAndVisibleAfterCommit) {
  run_on_both([](kv::KVStore &store) {
    auto a = store.connect();
    auto b = store.connect();
    CHECK_EQ(a->put("k", "v1"), kv::Status::OK);

    CHECK_EQ(a->begin_transaction(), kv::Status::OK);
    CHECK_EQ(a->put("k", "v2"), kv::Status::OK);
    CHECK_EQ(a->put("only-a", "x"), kv::Status::OK);

    // A 读自己的写；B 只看得到已提交的 v1
    CHECK_EQ(read(*a, "k"), std::string("v2"));
    CHECK_EQ(read(*b, "k"), std::string("v1"));
    CHECK_EQ(read(*b, "only-a"), std::string("<missing>"));
    CHECK(!b->exists("only-a"));
    // B 的扫描也不会看到 A 缓冲里的行
    CHECK_EQ(scan_keys(*b).size(), size_t{1});

    CHECK_EQ(a->commit_transaction(), kv::Status::OK);
    CHECK_EQ(read(*b, "k"), std::string("v2"));
    CHECK_EQ(read(*b, "only-a"), std::string("x"));
    CHECK_EQ(scan_keys(*b).size(), size_t{2});
  });
}

TEST(Connections, SecondWriterIsBusyButReadersAreNotBlocked) {
  run_on_both([](kv::KVStore &store) {
    auto a = store.connect();
    auto b = store.connect();
    CHECK_EQ(a->put("k", "v1"), kv::Status::OK);

    CHECK_EQ(a->begin_transaction(), kv::Status::OK);
    CHECK_EQ(a->put("k", "v2"), kv::Status::OK);
    CHECK(store.write_slot_held());

    // 第二个写事务拿不到写槽；自动提交的引擎级写也一样排队
    CHECK_EQ(b->begin_transaction(), kv::Status::Busy);
    CHECK_EQ(b->put("k", "v3"), kv::Status::Busy);
    CHECK_EQ(b->remove("k"), kv::Status::Busy);

    // 读者不受影响：读到的是已提交的旧值
    CHECK_EQ(read(*b, "k"), std::string("v1"));
    CHECK_EQ(scan_keys(*b).size(), size_t{1});

    // 事务结束（回滚）-> 写槽归还 -> 另一条连接可以写了
    CHECK_EQ(a->rollback_transaction(), kv::Status::OK);
    CHECK(!store.write_slot_held());
    CHECK_EQ(read(*b, "k"), std::string("v1"));
    CHECK_EQ(b->begin_transaction(), kv::Status::OK);
    CHECK_EQ(b->put("k", "v4"), kv::Status::OK);
    CHECK_EQ(b->commit_transaction(), kv::Status::OK);
    CHECK_EQ(read(*a, "k"), std::string("v4"));
  });
}

TEST(Connections, ScanIsNotAffectedByAnotherConnectionsCommit) {
  run_on_both([](kv::KVStore &store) {
    auto a = store.connect();
    auto b = store.connect();
    CHECK_EQ(a->put("k1", "1"), kv::Status::OK);
    CHECK_EQ(a->put("k3", "3"), kv::Status::OK);

    // A 打开一条扫描（迭代器创建 = 版本固定）
    auto it = a->new_iterator(kv::KeyRange::all());
    CHECK(it != nullptr);
    if (it == nullptr) {
      return;
    }

    // 扫描进行中，B 提交了新数据
    CHECK_EQ(b->put("k2", "2"), kv::Status::OK);

    std::vector<std::string> keys;
    for (it->seek_to_first(); it->valid(); it->next()) {
      keys.push_back(it->key());
    }
    // 这条扫描看不到 k2；但下一条扫描（新迭代器）看得到
    CHECK_EQ(keys.size(), size_t{2});
    if (keys.size() == 2) {
      CHECK_EQ(keys[0], std::string("k1"));
      CHECK_EQ(keys[1], std::string("k3"));
    }
    CHECK_EQ(scan_keys(*a).size(), size_t{3});
  });
}

TEST(Connections, ReverseScanWorksOnBothEngines) {
  run_on_both([](kv::KVStore &store) {
    auto conn = store.connect();
    for (const char *key : {"a", "b", "c"}) {
      CHECK_EQ(conn->put(key, key), kv::Status::OK);
    }

    kv::KeyRange range = kv::KeyRange::all();
    range.direction = kv::ScanDirection::kReverse;
    auto it = conn->new_iterator(range);
    CHECK(it != nullptr);
    if (it == nullptr) {
      return;
    }
    std::vector<std::string> keys;
    for (it->seek_to_first(); it->valid(); it->next()) {
      keys.push_back(it->key());
    }
    CHECK_EQ(keys.size(), size_t{3});
    if (keys.size() == 3) {
      CHECK_EQ(keys[0], std::string("c"));
      CHECK_EQ(keys[1], std::string("b"));
      CHECK_EQ(keys[2], std::string("a"));
    }
  });
}

TEST(Connections, ClosingAConnectionRollsBackAndReleasesTheWriteSlot) {
  run_on_both([](kv::KVStore &store) {
    {
      auto a = store.connect();
      CHECK_EQ(a->begin_transaction(), kv::Status::OK);
      CHECK_EQ(a->put("k", "v"), kv::Status::OK);
      CHECK(store.write_slot_held());
      // a 带着未提交事务析构：= 回滚 + 归还写槽
    }
    CHECK(!store.write_slot_held());

    auto b = store.connect();
    CHECK_EQ(b->begin_transaction(), kv::Status::OK);
    CHECK_EQ(b->rollback_transaction(), kv::Status::OK);
    CHECK(!b->exists("k")); // 事务没提交过：DB 干净
  });
}
