#pragma once

#include <memory>

#include "kv_engine.h"

namespace kv {
// ============================================================
// 引擎工厂（仅声明，具体实现在 .cpp 中）
//
// 用法（一个进程一份存储，一个 session 一条连接）：
//
//   auto store = kv::open_store(kv::EngineType::MOCK, options);
//   auto conn  = store->connect();          // = 一条连接（= 一个 session）
//   auto conn2 = store->connect();          // 第二条连接，共享同一份存储
// ============================================================

enum class EngineType { MOCK, LEVELDB, ROCKSDB };

// 只创建存储（还没打开）：需要"未打开的存储"来测错误路径时用它
std::shared_ptr<KVStore> create_store(EngineType type);

// 创建并打开存储；打开失败返回 nullptr
std::shared_ptr<KVStore> open_store(EngineType type,
                                    const DatabaseOptions &options);

} // namespace kv
