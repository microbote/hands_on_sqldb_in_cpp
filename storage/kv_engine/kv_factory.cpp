#include "kv_factory.h"
#include "storage/mock_engine/mock_engine.h"
#include "storage/leveldb_engine/leveldb_engine.h"

namespace kv{

std::unique_ptr<KVEngine> KVEngineFactory::create(EngineType type) {
  switch (type) {
    case EngineType::MOCK:
      return std::make_unique<MockEngine>();
    case EngineType::LEVELDB:
      return std::make_unique<LevelDBEngine>();
    default:
      break;
  }
  return nullptr;
}

}// namespace kv