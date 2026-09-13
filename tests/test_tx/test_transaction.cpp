// tests/test_tx/test_transaction.cpp
//
// 事务（悲观单写者 + 缓冲后一次提交）的引擎级行为：
//   - 事务期间 DB 不动（读穿只看得到自己的缓冲）；
//   - 提交 = 一个 WriteBatch（原子 + 落盘）；
//   - 回滚 = 丢弃缓冲，DB 从没被动过。
#include "test_framework.h"

#include <memory>
#include <string>
#include <vector>

#include "storage/kv_engine/tx_buffer.h"
#include "storage/mock_engine/mock_engine.h"

#if defined(SQLDB_HAVE_LEVELDB)
#include "storage/kv_engine/kv_factory.h"
#include "storage/leveldb_engine/leveldb_engine.h"
#include <cstdio>
#endif

namespace {

std::shared_ptr<kv::MockEngine> open_engine() {
  kv::DatabaseOptions options;
  options.path = "mock://tx-test";
  auto store = kv::open_store(kv::EngineType::MOCK, options);
  return std::static_pointer_cast<kv::MockEngine>(store->connect());
}

bool has(kv::MockEngine &engine, const std::string &key) {
  std::string value;
  return engine.get(key, &value) == kv::Status::OK;
}

std::string value_of(kv::MockEngine &engine, const std::string &key) {
  std::string value;
  engine.get(key, &value);
  return value;
}

} // namespace

TEST(Tx, BufferedWritesAreInvisibleToTheDbUntilCommit) {
  auto engine = open_engine();
  CHECK(engine->put("seed", "v0") == kv::Status::OK);

  CHECK(engine->begin_transaction() == kv::Status::OK);
  CHECK(engine->put("a", "1") == kv::Status::OK);
  CHECK(engine->put("b", "2") == kv::Status::OK);
  CHECK(engine->remove("seed") == kv::Status::OK);

  // 事务内读自己的写（读穿）
  CHECK(has(*engine, "a"));
  CHECK_EQ(value_of(*engine, "a"), std::string("1"));
  CHECK(!has(*engine, "seed")); // 本事务删了 -> 就是不存在

  // 但 DB 本体没被动过：size() 绕过缓冲直接看底层
  CHECK_EQ(engine->size(), size_t{1});

  CHECK(engine->commit_transaction() == kv::Status::OK);
  CHECK_EQ(engine->size(), size_t{2}); // seed 没了，a/b 进去了
  CHECK_EQ(value_of(*engine, "b"), std::string("2"));
  CHECK(!has(*engine, "seed"));
}

TEST(Tx, RollbackDiscardsEverythingAndDbIsUnchanged) {
  auto engine = open_engine();
  CHECK(engine->put("k", "old") == kv::Status::OK);

  CHECK(engine->begin_transaction() == kv::Status::OK);
  CHECK(engine->put("k", "new") == kv::Status::OK);
  CHECK(engine->put("added", "x") == kv::Status::OK);
  CHECK(engine->remove("k") == kv::Status::OK);
  CHECK(engine->rollback_transaction() == kv::Status::OK);

  // 一切照旧
  CHECK_EQ(value_of(*engine, "k"), std::string("old"));
  CHECK(!has(*engine, "added"));
  CHECK_EQ(engine->size(), size_t{1});

  // 回滚之后还能正常开新事务
  CHECK(engine->begin_transaction() == kv::Status::OK);
  CHECK(engine->put("k", "third") == kv::Status::OK);
  CHECK(engine->commit_transaction() == kv::Status::OK);
  CHECK_EQ(value_of(*engine, "k"), std::string("third"));
}

TEST(Tx, ReadYourOwnWritesInsideTheBuffer) {
  auto engine = open_engine();
  CHECK(engine->begin_transaction() == kv::Status::OK);

  // put -> remove -> put：最后一次写生效
  CHECK(engine->put("k", "v1") == kv::Status::OK);
  CHECK_EQ(value_of(*engine, "k"), std::string("v1"));
  CHECK(engine->remove("k") == kv::Status::OK);
  CHECK(!has(*engine, "k"));
  CHECK(engine->put("k", "v2") == kv::Status::OK);
  CHECK_EQ(value_of(*engine, "k"), std::string("v2"));
  CHECK(engine->exists("k"));

  CHECK(engine->commit_transaction() == kv::Status::OK);
  CHECK_EQ(value_of(*engine, "k"), std::string("v2"));
  CHECK_EQ(engine->size(), size_t{1}); // 中间态没有留在 DB 里
}

TEST(Tx, ScanSkipsRowsDeletedInTheTransaction) {
  auto engine = open_engine();
  for (const char *key : {"a", "b", "c"}) {
    CHECK(engine->put(key, "old") == kv::Status::OK);
  }

  CHECK(engine->begin_transaction() == kv::Status::OK);
  CHECK(engine->remove("b") == kv::Status::OK);
  CHECK(engine->put("c", "new") == kv::Status::OK);

  kv::KeyRange range;
  auto it = engine->new_iterator(range);
  CHECK(it != nullptr);
  std::vector<std::pair<std::string, std::string>> seen;
  while (it->valid()) {
    seen.emplace_back(it->key(), it->value());
    it->next();
  }
  // b 被本事务删了 -> 不出现；c 的值是本事务改的新值
  CHECK_EQ(seen.size(), size_t{2});
  if (seen.size() == 2) {
    CHECK_EQ(seen[0].first, std::string("a"));
    CHECK_EQ(seen[1].first, std::string("c"));
    CHECK_EQ(seen[1].second, std::string("new"));
  }

  CHECK(engine->rollback_transaction() == kv::Status::OK);
  // 回滚后 b 还在、c 还是旧值
  CHECK(has(*engine, "b"));
  CHECK_EQ(value_of(*engine, "c"), std::string("old"));
}

TEST(Tx, ReverseScanAlsoAppliesTheOverlay) {
  auto engine = open_engine();
  for (const char *key : {"a", "b", "c"}) {
    CHECK(engine->put(key, "old") == kv::Status::OK);
  }
  CHECK(engine->begin_transaction() == kv::Status::OK);
  CHECK(engine->remove("c") == kv::Status::OK);
  CHECK(engine->put("b", "new") == kv::Status::OK);

  kv::KeyRange range;
  range.direction = kv::ScanDirection::kReverse;
  auto it = engine->new_iterator(range);
  std::vector<std::string> keys;
  while (it->valid()) {
    keys.push_back(it->key());
    it->next();
  }
  CHECK_EQ(keys.size(), size_t{2});
  if (keys.size() == 2) {
    CHECK_EQ(keys[0], std::string("b")); // c 被删了
    CHECK_EQ(keys[1], std::string("a"));
  }
  CHECK(engine->rollback_transaction() == kv::Status::OK);
}

// ============================================================
// 合并迭代器：事务内**新插入**的行也要能被扫到（多语句事务的前提）
// ============================================================
namespace {

std::vector<std::string> scan_keys(kv::KVEngine &engine,
                                   kv::ScanDirection direction) {
  kv::KeyRange range;
  range.direction = direction;
  auto it = engine.new_iterator(range);
  std::vector<std::string> keys;
  while (it != nullptr && it->valid()) {
    keys.push_back(it->key());
    it->next();
  }
  return keys;
}

} // namespace

TEST(Tx, ForwardScanMergesInsertedKeysInOrder) {
  auto engine = open_engine();
  CHECK(engine->put("b", "db-b") == kv::Status::OK);
  CHECK(engine->put("d", "db-d") == kv::Status::OK);

  CHECK(engine->begin_transaction() == kv::Status::OK);
  CHECK(engine->put("a", "tx-a") == kv::Status::OK); // 新插入
  CHECK(engine->put("c", "tx-c") == kv::Status::OK); // 新插入
  CHECK(engine->put("b", "tx-b") == kv::Status::OK); // 覆盖已有

  const auto keys = scan_keys(*engine, kv::ScanDirection::kForward);
  CHECK_EQ(keys.size(), size_t{4});
  if (keys.size() == 4) {
    CHECK_EQ(keys[0], std::string("a"));
    CHECK_EQ(keys[1], std::string("b"));
    CHECK_EQ(keys[2], std::string("c"));
    CHECK_EQ(keys[3], std::string("d"));
  }
  CHECK_EQ(value_of(*engine, "b"), std::string("tx-b")); // 覆盖后的值
  CHECK(engine->rollback_transaction() == kv::Status::OK);
  // 回滚后新插入的都没了，覆盖的恢复旧值
  CHECK_EQ(scan_keys(*engine, kv::ScanDirection::kForward).size(), size_t{2});
  CHECK_EQ(value_of(*engine, "b"), std::string("db-b"));
}

TEST(Tx, ReverseScanMergesInsertedKeysInOrder) {
  auto engine = open_engine();
  CHECK(engine->put("b", "1") == kv::Status::OK);
  CHECK(engine->put("d", "2") == kv::Status::OK);
  CHECK(engine->begin_transaction() == kv::Status::OK);
  CHECK(engine->put("c", "3") == kv::Status::OK); // 新插入
  CHECK(engine->remove("d") == kv::Status::OK);   // 删掉已存在的

  const auto keys = scan_keys(*engine, kv::ScanDirection::kReverse);
  CHECK_EQ(keys.size(), size_t{2});
  if (keys.size() == 2) {
    CHECK_EQ(keys[0], std::string("c")); // 新插入的也要出现在反向扫描里
    CHECK_EQ(keys[1], std::string("b"));
  }
  CHECK(engine->rollback_transaction() == kv::Status::OK);
}

TEST(Tx, ScanRespectsRangeBoundsWithOverlay) {
  auto engine = open_engine();
  CHECK(engine->put("b", "1") == kv::Status::OK);
  CHECK(engine->put("d", "2") == kv::Status::OK);
  CHECK(engine->put("f", "3") == kv::Status::OK);
  CHECK(engine->begin_transaction() == kv::Status::OK);
  CHECK(engine->put("c", "4") == kv::Status::OK); // 落在区间内
  CHECK(engine->put("e", "5") == kv::Status::OK); // 落在区间内
  CHECK(engine->put("z", "6") == kv::Status::OK); // 区间外

  kv::KeyRange range;
  range.start = "b";
  range.end = "f"; // [b, f)
  auto it = engine->new_iterator(range);
  std::vector<std::string> keys;
  while (it->valid()) {
    keys.push_back(it->key());
    it->next();
  }
  CHECK_EQ(keys.size(), size_t{4});
  if (keys.size() == 4) {
    CHECK_EQ(keys[0], std::string("b"));
    CHECK_EQ(keys[1], std::string("c"));
    CHECK_EQ(keys[2], std::string("d"));
    CHECK_EQ(keys[3], std::string("e"));
  }
  CHECK(engine->rollback_transaction() == kv::Status::OK);
}

TEST(Tx, SeekAndSeekToLastIncludeOverlayKeys) {
  auto engine = open_engine();
  CHECK(engine->put("b", "1") == kv::Status::OK);
  CHECK(engine->begin_transaction() == kv::Status::OK);
  CHECK(engine->put("c", "2") == kv::Status::OK); // 新插入
  CHECK(engine->put("d", "3") == kv::Status::OK); // 新插入

  kv::KeyRange range;
  auto it = engine->new_iterator(range);
  it->seek("c");
  CHECK(it->valid());
  if (it->valid()) {
    CHECK_EQ(it->key(), std::string("c")); // 覆盖层里的 key 也能被 seek 到
    it->next();
    CHECK_EQ(it->key(), std::string("d"));
    it->prev(); // 回到 c
    CHECK_EQ(it->key(), std::string("c"));
  }

  it = engine->new_iterator(range);
  it->seek_to_last();
  CHECK(it->valid());
  if (it->valid()) {
    CHECK_EQ(it->key(), std::string("d")); // 最后一项来自覆盖层
  }
  CHECK(engine->rollback_transaction() == kv::Status::OK);
}

TEST(Tx, OnlyOneWriteTransactionAtATime) {
  auto engine = open_engine();
  CHECK(engine->begin_transaction() == kv::Status::OK);
  CHECK(engine->in_transaction());
  CHECK(engine->begin_transaction() == kv::Status::Busy); // 悲观单写者
  CHECK(engine->rollback_transaction() == kv::Status::OK);
  CHECK(!engine->in_transaction());
  CHECK(engine->begin_transaction() == kv::Status::OK);
  CHECK(engine->commit_transaction() == kv::Status::OK);
}

TEST(Tx, CommitAndRollbackWithoutTransactionAreRejected) {
  auto engine = open_engine();
  CHECK(engine->commit_transaction() == kv::Status::NotFound);
  CHECK(engine->rollback_transaction() == kv::Status::NotFound);
}

TEST(Tx, FailedCommitLeavesNothingBehindAndKeepsBufferForRetry) {
  auto engine = open_engine();
  CHECK(engine->begin_transaction() == kv::Status::OK);
  CHECK(engine->put("a", "1") == kv::Status::OK);
  CHECK(engine->put("b", "2") == kv::Status::OK);

  engine->set_fail_writes(true);
  CHECK(engine->commit_transaction() != kv::Status::OK);
  CHECK_EQ(engine->size(), size_t{0}); // 一条都没落
  CHECK(engine->in_transaction());     // 缓冲还在 -> 可以重试

  engine->set_fail_writes(false);
  CHECK(engine->commit_transaction() == kv::Status::OK); // 重试成功
  CHECK_EQ(engine->size(), size_t{2});
}

TEST(Tx, RemoveRangeIsAtomicAndOrderSensitive) {
  auto engine = open_engine();
  CHECK(engine->begin_transaction() == kv::Status::OK);
  // 先写一个落在区间里的 key，再整段删：后者的序号更大 -> 删掉
  CHECK(engine->put("b", "in-range") == kv::Status::OK);
  kv::WriteBatch batch;
  batch.remove_range("a", "d");
  CHECK(engine->write_batch(batch) == kv::Status::OK); // 事务里路由进缓冲
  CHECK(!has(*engine, "b"));
  CHECK(engine->commit_transaction() == kv::Status::OK);
  CHECK_EQ(engine->size(), size_t{0});

  // 反过来：先整段删，再写同一个 key -> 写生效
  CHECK(engine->begin_transaction() == kv::Status::OK);
  kv::WriteBatch batch2;
  batch2.remove_range("a", "d");
  CHECK(engine->write_batch(batch2) == kv::Status::OK);
  CHECK(engine->put("b", "after") == kv::Status::OK);
  CHECK(engine->commit_transaction() == kv::Status::OK);
  CHECK_EQ(value_of(*engine, "b"), std::string("after"));
}

TEST(Tx, BatchIsAtomicEvenWhenOneOpIsInvalid) {
  auto engine = open_engine();
  // 反区间（end <= begin）是非法的：整批必须被拒绝，不能写一半
  kv::WriteBatch reverse_range;
  reverse_range.put("ok", "1");
  reverse_range.remove_range("z", "a");
  CHECK(engine->write_batch(reverse_range) == kv::Status::InvalidArgument);
  CHECK_EQ(engine->size(), size_t{0}); // 没有半截写入
}

TEST(Tx, BufferReportsItsSizeAndLimit) {
  kv::TxBuffer buffer;
  CHECK(buffer.empty());
  buffer.put("k", std::string(100, 'x'));
  buffer.remove("k2");
  buffer.remove_range("a", "z");
  CHECK(!buffer.empty());
  CHECK_EQ(buffer.op_count(), size_t{3});
  CHECK(buffer.bytes() > 0);
  CHECK(!buffer.exceeds(1u << 20));
  CHECK(buffer.exceeds(16)); // 很小的上限立刻超

  // 视图：最后一条操作生效；range 删除的序号大于前面的 put -> 删除
  CHECK(buffer.lookup("k").is_tombstone());
  CHECK(buffer.lookup("k2").is_tombstone());
  CHECK(!buffer.lookup("zzz").covered());

  // 提交时按 op 顺序生成 batch
  const kv::WriteBatch batch = buffer.to_batch();
  CHECK_EQ(batch.size(), size_t{3});
  CHECK(batch.ops()[0].type == kv::WriteBatch::OpType::kPut);
  CHECK(batch.ops()[1].type == kv::WriteBatch::OpType::kRemove);
  CHECK(batch.ops()[2].type == kv::WriteBatch::OpType::kRemoveRange);
}

// ============================================================
// 引擎一致性：同一套语义在两个引擎上都要成立
//
// 这一组是"血泪教训"——LevelDB 侧曾经在 commit 里持锁重入（死锁），
// 而当时只有 Mock 引擎进了测试。两个引擎必须跑同一套用例。
// ============================================================
#if defined(SQLDB_HAVE_LEVELDB)

namespace {

// 每个用例用独立目录（避免上一个进程留下的 LOCK 影响）
std::string temp_db_path(const char *name) {
  return std::string("/tmp/sqldb_tx_test_") + name;
}

kv::Status run_on_leveldb(const char *name,
                          const std::function<void(kv::KVEngine &)> &body) {
  const std::string path = temp_db_path(name);
  std::remove(path.c_str());
  kv::DatabaseOptions options;
  options.set_path(path).set_create_if_missing(true).set_error_if_exists(false);
  auto store = kv::open_store(kv::EngineType::LEVELDB, options);
  CHECK(store != nullptr);
  if (store == nullptr) {
    return kv::Status::IOError;
  }
  auto engine = store->connect();
  body(*engine);
  engine.reset();
  store->close();
  return kv::Status::OK;
}

} // namespace

TEST(TxLevelDb, CommitAndRollbackBehaveLikeMock) {
  run_on_leveldb("commit", [](kv::KVEngine &engine) {
    CHECK(engine.put("seed", "v0") == kv::Status::OK);
    CHECK(engine.begin_transaction() == kv::Status::OK);
    CHECK(engine.put("a", "1") == kv::Status::OK);
    CHECK(engine.remove("seed") == kv::Status::OK);
    CHECK(engine.commit_transaction() == kv::Status::OK); // 这里曾经死锁

    std::string value;
    CHECK(engine.get("a", &value) == kv::Status::OK);
    CHECK_EQ(value, std::string("1"));
    CHECK(engine.get("seed", &value) == kv::Status::NotFound);

    // 回滚：什么都不写
    CHECK(engine.begin_transaction() == kv::Status::OK);
    CHECK(engine.put("b", "2") == kv::Status::OK);
    CHECK(engine.rollback_transaction() == kv::Status::OK);
    CHECK(engine.get("b", &value) == kv::Status::NotFound);
  });
}

TEST(TxLevelDb, ReadThroughAndScanOverlay) {
  run_on_leveldb("scan", [](kv::KVEngine &engine) {
    CHECK(engine.put("a", "old") == kv::Status::OK);
    CHECK(engine.put("b", "old") == kv::Status::OK);
    CHECK(engine.begin_transaction() == kv::Status::OK);
    CHECK(engine.put("a", "new") == kv::Status::OK);
    CHECK(engine.remove("b") == kv::Status::OK);

    std::string value;
    CHECK(engine.get("a", &value) == kv::Status::OK);
    CHECK_EQ(value, std::string("new")); // 读穿
    CHECK(engine.get("b", &value) == kv::Status::NotFound);

    // 扫描也看得到覆盖视图（删掉的跳过、改过的给新值）
    kv::KeyRange range;
    auto it = engine.new_iterator(range);
    CHECK(it != nullptr);
    std::vector<std::pair<std::string, std::string>> seen;
    while (it->valid()) {
      seen.emplace_back(it->key(), it->value());
      it->next();
    }
    CHECK_EQ(seen.size(), size_t{1});
    if (!seen.empty()) {
      CHECK_EQ(seen[0].first, std::string("a"));
      CHECK_EQ(seen[0].second, std::string("new"));
    }
    CHECK(engine.rollback_transaction() == kv::Status::OK);
  });
}

TEST(TxLevelDb, RemoveRangeDeletesPrefix) {
  run_on_leveldb("range", [](kv::KVEngine &engine) {
    CHECK(engine.put("p/a", "1") == kv::Status::OK);
    CHECK(engine.put("p/b", "2") == kv::Status::OK);
    CHECK(engine.put("q/a", "3") == kv::Status::OK);
    kv::WriteBatch batch;
    batch.remove_range("p/", "p0"); // prefix_end("p/")
    CHECK(engine.write_batch(batch) == kv::Status::OK);

    std::string value;
    CHECK(engine.get("p/a", &value) == kv::Status::NotFound);
    CHECK(engine.get("p/b", &value) == kv::Status::NotFound);
    CHECK(engine.get("q/a", &value) == kv::Status::OK);
  });
}

#endif // SQLDB_HAVE_LEVELDB
