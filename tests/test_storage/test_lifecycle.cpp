// tests/test_storage/test_lifecycle.cpp
//
// 存储与连接的生命周期：打开/关闭/重复打开、未打开时的操作、连接与写槽、
// leveldb 重开同一个库数据还在（持久化）、mock 的故障注入。
#include "test_framework.h"

#include "storage/mock_engine/mock_engine.h"
#include "storage_test_util.h"

#include <memory>
#include <string>

TEST(StoreLifecycle, OpenCloseAndReopenOnBothEngines) {
  storagetest::run_on_both("lifecycle", [](kv::KVStore &store) {
    CHECK(store.is_open());
    CHECK(!store.name().empty());

    auto conn = store.connect();
    CHECK(conn->is_open());
    CHECK_EQ(conn->put("k", "v"), kv::Status::OK);
    CHECK(conn->store() != nullptr);      // 连接挂在存储上
    CHECK(conn->store().get() == &store); // 就是这一份

    // 关掉存储：连接的操作立刻失败（不再是"看起来还能用"）
    CHECK_EQ(store.close(), kv::Status::OK);
    CHECK(!store.is_open());
    CHECK(!conn->is_open());
    kv::ByteValue value;
    CHECK(conn->put("k2", "v2") != kv::Status::OK);
    CHECK(conn->get("k", &value) != kv::Status::OK);

    // 重复关闭返回 NotFound
    CHECK_EQ(store.close(), kv::Status::NotFound);
  });
}

TEST(StoreLifecycle, DoubleOpenIsRejected) {
  storagetest::run_on_both("double-open", [](kv::KVStore &store) {
    kv::DatabaseOptions again;
    again.set_path("mock://double-open");
    CHECK_EQ(store.open(again), kv::Status::AlreadyExists);
  });
}

TEST(StoreLifecycle, ConnectionsShareTheStoreAndEachHasItsOwnTransaction) {
  storagetest::run_on_both("connections", [](kv::KVStore &store) {
    auto a = store.connect();
    auto b = store.connect();
    CHECK(a->store().get() == b->store().get()); // 同一条存储
    CHECK(a->store().get() == &store);

    CHECK_EQ(a->put("k", "v1"), kv::Status::OK);
    CHECK_EQ(storagetest::value_of(*b, "k"), std::string("v1"));

    // A 的事务只影响 A
    CHECK_EQ(a->begin_transaction(), kv::Status::OK);
    CHECK_EQ(a->put("k", "v2"), kv::Status::OK);
    CHECK_EQ(storagetest::value_of(*a, "k"), std::string("v2"));
    CHECK_EQ(storagetest::value_of(*b, "k"), std::string("v1"));
    CHECK_EQ(a->rollback_transaction(), kv::Status::OK);
    CHECK_EQ(storagetest::value_of(*a, "k"), std::string("v1"));
  });
}

#if defined(SQLDB_HAVE_LEVELDB)
TEST(StoreLifecycle, LevelDbKeepsDataAcrossReopen) {
  const std::string path = storagetest::fresh_leveldb_path("persist");
  {
    auto store = storagetest::open_leveldb_at(path);
    CHECK(store != nullptr);
    if (store == nullptr) {
      return;
    }
    auto conn = store->connect();
    CHECK_EQ(conn->put("persisted", "yes"), kv::Status::OK);
    CHECK_EQ(store->close(), kv::Status::OK);
  }
  // 重开同一个目录：数据还在（这是"提交即持久"的端到端证据）
  {
    auto store = storagetest::open_leveldb_at(path);
    CHECK(store != nullptr);
    if (store == nullptr) {
      return;
    }
    auto conn = store->connect();
    CHECK_EQ(storagetest::value_of(*conn, "persisted"), std::string("yes"));
    CHECK_EQ(store->close(), kv::Status::OK);
  }
}
#endif

TEST(StoreLifecycle, MockFaultInjectionOnlyAffectsBatchWrites) {
  storagetest::run_on_mock("fault", [](kv::KVStore &store) {
    auto conn = store.connect();
    CHECK_EQ(conn->put("before", "1"), kv::Status::OK);

    auto *mock = dynamic_cast<kv::MockStore *>(&store);
    CHECK(mock != nullptr);
    if (mock == nullptr) {
      return;
    }
    mock->set_fail_writes(true);

    kv::WriteBatch batch;
    batch.put("after", "2");
    CHECK(conn->write_batch(batch) != kv::Status::OK); // 批量写失败
    CHECK(!conn->exists("after"));
    CHECK(conn->exists("before"));
    // 故障注入只挡批量写（单键 put 不走 apply_batch_locked）——事务提交走批量，
    // 所以 session 层看到的就是"提交失败、一条都不落"（见 test_tx）
    CHECK_EQ(conn->put("single", "3"), kv::Status::OK);

    mock->set_fail_writes(false);
    CHECK_EQ(conn->write_batch(batch), kv::Status::OK);
    CHECK(conn->exists("after"));
  });
}
