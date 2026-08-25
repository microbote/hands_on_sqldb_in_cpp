#include "storage_engine.h"
#include "mock_engine.h"
#include "leveldb_engine.h"

namespace storage {

std::unique_ptr<StorageEngine> StorageEngineFactory::create_engine(
    const StorageOptions& options) {
  switch (options.type) {
    case StorageType::MOCK:
      return std::make_unique<MockEngine>();
    case StorageType::LEVELDB:
      if (options.path.empty()) {
        throw std::runtime_error("LevelDB path is empty");
      }
      return std::make_unique<LevelDBEngine>(options.path);
    default:
      return std::make_unique<MockEngine>();
  }
}

}  // namespace storage