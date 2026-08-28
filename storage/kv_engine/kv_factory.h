#pragma once

#include "kv_engine.h"

namespace kv {
// ============================================================
// 引擎工厂（仅声明，具体实现在 .cpp 中）
// ============================================================

enum class EngineType { MOCK, LEVELDB, ROCKSDB };

class KVEngineFactory {
 public:
  static std::unique_ptr<KVEngine> create(EngineType type);
};

}  // namespace kv