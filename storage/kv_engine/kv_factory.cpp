#include "kv_factory.h"
#include "storage/leveldb_engine/leveldb_engine.h"
#include "storage/mock_engine/mock_engine.h"

namespace kv {

std::shared_ptr<KVStore> create_store(EngineType type) {
  switch (type) {
  case EngineType::MOCK:
    return std::make_shared<MockStore>();
  case EngineType::LEVELDB:
    return std::make_shared<LevelDBStore>();
  default:
    break;
  }
  return nullptr;
}

std::shared_ptr<KVStore> open_store(EngineType type,
                                    const DatabaseOptions &options) {
  auto store = create_store(type);
  if (store == nullptr) {
    return nullptr;
  }
  if (store->open(options) != Status::OK) {
    return nullptr;
  }
  return store;
}

} // namespace kv
