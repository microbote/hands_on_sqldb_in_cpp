// tests/test_storage/test_kv_basics.cpp
//
// 单键与批量操作的基本语义（借用旧 tests/test_mock_engine.cpp 的用例，
// 补上"两个引擎必须一致"和缺失 key 的边界）。每个用例跑 Mock + LevelDB。
#include "test_framework.h"

#include "storage_test_util.h"

#include <string>
#include <vector>

TEST(KvBasics, PutGetRemoveRoundTrip) {
  storagetest::run_on_both("basics", [](kv::KVStore &store) {
    auto conn = store.connect();

    CHECK_EQ(conn->put("key1", "value1"), kv::Status::OK);
    CHECK_EQ(conn->put("key2", "value2"), kv::Status::OK);
    CHECK_EQ(storagetest::value_of(*conn, "key1"), std::string("value1"));
    CHECK_EQ(storagetest::value_of(*conn, "key2"), std::string("value2"));
    CHECK(conn->exists("key1"));

    // 覆盖写
    CHECK_EQ(conn->put("key1", "value1b"), kv::Status::OK);
    CHECK_EQ(storagetest::value_of(*conn, "key1"), std::string("value1b"));

    CHECK_EQ(conn->remove("key2"), kv::Status::OK);
    CHECK(!conn->exists("key2"));
    CHECK_EQ(conn->get("key2", nullptr), kv::Status::NotFound);
  });
}

TEST(KvBasics, MissingKeySemanticsAreIdenticalOnBothEngines) {
  storagetest::run_on_both("missing", [](kv::KVStore &store) {
    auto conn = store.connect();
    kv::ByteValue value;

    // 读不存在的 key
    CHECK_EQ(conn->get("nope", &value), kv::Status::NotFound);
    CHECK(!conn->exists("nope"));
    // **删不存在的 key：两个引擎都返回 NotFound**（leveldb 原生是 OK，
    // 这里刻意多查一遍以对齐 Mock；事务里则是幂等的，见 test_tx）
    CHECK_EQ(conn->remove("nope"), kv::Status::NotFound);
    // 删两次：第二次一定 NotFound
    CHECK_EQ(conn->put("k", "v"), kv::Status::OK);
    CHECK_EQ(conn->remove("k"), kv::Status::OK);
    CHECK_EQ(conn->remove("k"), kv::Status::NotFound);
  });
}

TEST(KvBasics, OverwriteKeepsKeyCount) {
  storagetest::run_on_both("overwrite", [](kv::KVStore &store) {
    auto conn = store.connect();
    for (int i = 0; i < 5; ++i) {
      CHECK_EQ(conn->put("same", "v" + std::to_string(i)), kv::Status::OK);
    }
    CHECK_EQ(storagetest::scan_keys(*conn).size(), size_t{1});
    CHECK_EQ(storagetest::value_of(*conn, "same"), std::string("v4"));
  });
}

TEST(KvBasics, WriteBatchAppliesPutsAndRemoves) {
  storagetest::run_on_both("batch", [](kv::KVStore &store) {
    auto conn = store.connect();
    CHECK_EQ(conn->put("keep", "1"), kv::Status::OK);
    CHECK_EQ(conn->put("drop", "2"), kv::Status::OK);

    kv::WriteBatch batch;
    batch.put("new1", "n1");
    batch.put("new2", "n2");
    batch.remove("drop");
    CHECK_EQ(conn->write_batch(batch), kv::Status::OK);

    CHECK(conn->exists("new1"));
    CHECK(conn->exists("new2"));
    CHECK(!conn->exists("drop"));
    CHECK_EQ(storagetest::value_of(*conn, "keep"), std::string("1"));
  });
}

TEST(KvBasics, InvalidOpRejectsTheWholeBatch) {
  storagetest::run_on_both("bad-batch", [](kv::KVStore &store) {
    auto conn = store.connect();

    kv::WriteBatch batch;
    batch.put("good", "1");
    batch.remove_range("z", "a"); // 非法 op：反区间（end <= begin）
    CHECK_EQ(conn->write_batch(batch), kv::Status::InvalidArgument);

    // 一条都不落（和 LevelDB 的 WriteBatch 原子性对齐）
    CHECK(!conn->exists("good"));
    CHECK_EQ(storagetest::scan_keys(*conn).size(), size_t{0});
  });
}

TEST(KvBasics, GetBatchHonoursMissingKeyPolicy) {
  storagetest::run_on_both("get-batch", [](kv::KVStore &store) {
    auto conn = store.connect();
    CHECK_EQ(conn->put("a", "1"), kv::Status::OK);

    std::vector<kv::Key> keys{"a", "missing"};
    std::vector<std::optional<kv::ByteValue>> values;

    // 允许缺失：空位用 nullopt 表示
    CHECK_EQ(conn->get_batch(keys, kv::MissingKeyPolicy::kReturnEmpty, &values),
             kv::Status::OK);
    CHECK_EQ(values.size(), size_t{2});
    if (values.size() == 2) {
      CHECK(values[0].has_value());
      CHECK_EQ(values[0].value_or(""), std::string("1"));
      CHECK(!values[1].has_value());
    }

    // 不允许缺失：整个调用报 NotFound
    values.clear();
    CHECK_EQ(conn->get_batch(keys, kv::MissingKeyPolicy::kReturnError, &values),
             kv::Status::NotFound);
  });
}

TEST(KvBasics, RemoveRangeDeletesHalfOpenInterval) {
  storagetest::run_on_both("remove-range", [](kv::KVStore &store) {
    auto conn = store.connect();
    for (const char *key : {"a", "b", "c", "d"}) {
      CHECK_EQ(conn->put(key, key), kv::Status::OK);
    }

    kv::WriteBatch batch;
    batch.remove_range("a", "c"); // [a, c)
    CHECK_EQ(conn->write_batch(batch), kv::Status::OK);

    const auto keys = storagetest::scan_keys(*conn);
    CHECK_EQ(keys.size(), size_t{2});
    if (keys.size() == 2) {
      CHECK_EQ(keys[0], std::string("c"));
      CHECK_EQ(keys[1], std::string("d"));
    }
  });
}
