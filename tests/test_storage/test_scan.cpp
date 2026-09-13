// tests/test_storage/test_scan.cpp
//
// 扫描族：全扫、区间（左闭右开）、前缀、反向、seek/seek_to_last/prev、
// for_each、空区间。借用旧 tests/test_leveldb_engine.cpp 的用例，
// 并强制"两个引擎同一份断言"。
#include "test_framework.h"

#include "storage_test_util.h"

#include <string>
#include <vector>

namespace {

void seed_abcde(kv::KVEngine &conn) {
  for (const char *key : {"a", "b", "c", "d", "e"}) {
    CHECK_EQ(conn.put(key, key), kv::Status::OK);
  }
}

std::vector<std::string> collect(kv::Iterator &it) {
  std::vector<std::string> keys;
  for (it.seek_to_first(); it.valid(); it.next()) {
    keys.push_back(it.key());
  }
  return keys;
}

} // namespace

TEST(Scan, FullAndRangeScanAreHalfOpen) {
  storagetest::run_on_both("scan-range", [](kv::KVStore &store) {
    auto conn = store.connect();
    seed_abcde(*conn);

    CHECK_EQ(storagetest::scan_keys(*conn).size(), size_t{5});

    // [b, d)：左闭右开
    auto it = conn->new_iterator(kv::KeyRange::range("b", "d"));
    CHECK(it != nullptr);
    if (it == nullptr) {
      return;
    }
    const auto keys = collect(*it);
    CHECK_EQ(keys.size(), size_t{2});
    if (keys.size() == 2) {
      CHECK_EQ(keys[0], std::string("b"));
      CHECK_EQ(keys[1], std::string("c"));
    }
  });
}

TEST(Scan, PrefixScanOnlyReturnsThatPrefix) {
  storagetest::run_on_both("prefix", [](kv::KVStore &store) {
    auto conn = store.connect();
    seed_abcde(*conn);
    CHECK_EQ(conn->put("ba", "ba"), kv::Status::OK);
    CHECK_EQ(conn->put("bb", "bb"), kv::Status::OK);
    CHECK_EQ(conn->put("zz", "zz"), kv::Status::OK);

    auto it = conn->new_prefix_iterator("b");
    CHECK(it != nullptr);
    if (it == nullptr) {
      return;
    }
    const auto keys = collect(*it);
    CHECK_EQ(keys.size(), size_t{3});
    for (const auto &key : keys) {
      CHECK(key.starts_with("b"));
    }
  });
}

TEST(Scan, ReverseScanVisitsKeysInDescendingOrder) {
  storagetest::run_on_both("reverse", [](kv::KVStore &store) {
    auto conn = store.connect();
    seed_abcde(*conn);

    kv::KeyRange range = kv::KeyRange::all();
    range.direction = kv::ScanDirection::kReverse;
    auto it = conn->new_iterator(range);
    CHECK(it != nullptr);
    if (it == nullptr) {
      return;
    }
    const auto keys = collect(*it);
    CHECK_EQ(keys.size(), size_t{5});
    if (keys.size() == 5) {
      CHECK_EQ(keys[0], std::string("e"));
      CHECK_EQ(keys[4], std::string("a"));
    }
  });
}

TEST(Scan, SeekAndSeekToLastRespectTheRange) {
  storagetest::run_on_both("seek", [](kv::KVStore &store) {
    auto conn = store.connect();
    seed_abcde(*conn);

    auto it = conn->new_iterator(kv::KeyRange::range("b", "e"));
    CHECK(it != nullptr);
    if (it == nullptr) {
      return;
    }

    // seek 到区间中间的 key
    it->seek("c");
    CHECK(it->valid());
    if (it->valid()) {
      CHECK_EQ(it->key(), std::string("c"));
    }

    // 区间上界（排他）之后 -> 无效
    it->seek("e");
    CHECK(!it->valid());

    // seek 到区间起点之前 -> 落在区间第一条（正向扫描）
    it->seek("a");
    CHECK(it->valid());
    if (it->valid()) {
      CHECK_EQ(it->key(), std::string("b"));
    }

    // seek_to_last / prev / seek_to_first
    it->seek_to_last();
    CHECK(it->valid());
    if (it->valid()) {
      CHECK_EQ(it->key(), std::string("d"));
    }
    it->prev();
    CHECK(it->valid());
    if (it->valid()) {
      CHECK_EQ(it->key(), std::string("c"));
    }
    it->seek_to_first();
    CHECK(it->valid());
    if (it->valid()) {
      CHECK_EQ(it->key(), std::string("b"));
    }
  });
}

TEST(Scan, EmptyRangeAndEmptyStoreScanYieldNothing) {
  storagetest::run_on_both("empty", [](kv::KVStore &store) {
    auto conn = store.connect();

    // 空库
    auto it = conn->new_iterator(kv::KeyRange::all());
    CHECK(it != nullptr);
    if (it != nullptr) {
      it->seek_to_first();
      CHECK(!it->valid());
      CHECK_EQ(it->status(), kv::Status::NotFound);
    }

    seed_abcde(*conn);
    // 空区间 [c, c)
    auto empty = conn->new_iterator(kv::KeyRange::range("c", "c"));
    CHECK(empty != nullptr);
    if (empty != nullptr) {
      CHECK_EQ(collect(*empty).size(), size_t{0});
    }
  });
}

TEST(Scan, ForEachStopsWhenCallbackReturnsFalse) {
  storagetest::run_on_both("for-each", [](kv::KVStore &store) {
    auto conn = store.connect();
    seed_abcde(*conn);

    auto it = conn->new_iterator(kv::KeyRange::all());
    CHECK(it != nullptr);
    if (it == nullptr) {
      return;
    }
    size_t visited = 0;
    it->for_each([&visited](const kv::Key &key, const kv::ByteValue &value) {
      CHECK_EQ(key, value); // seed 里 key == value
      ++visited;
      return visited < 2; // 返回 false = 提前停止
    });
    CHECK_EQ(visited, size_t{2});
  });
}

TEST(Scan, IteratorIsExhaustedOnlyOnce) {
  storagetest::run_on_both("exhausted", [](kv::KVStore &store) {
    auto conn = store.connect();
    CHECK_EQ(conn->put("only", "1"), kv::Status::OK);

    auto it = conn->new_iterator(kv::KeyRange::all());
    CHECK(it != nullptr);
    if (it == nullptr) {
      return;
    }
    it->seek_to_first();
    CHECK(it->valid());
    it->next();
    CHECK(!it->valid());
    CHECK_EQ(it->status(), kv::Status::NotFound);
    // 已结束之后再 next/prev 都是 no-op（两个引擎都先判 valid()），不会"复活"
    it->next();
    it->prev();
    CHECK(!it->valid());
    CHECK(!it->error_message().empty());
  });
}
