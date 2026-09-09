// value.cpp
#include "value.h"
#include "key.h"

namespace sql {

/*
这个实现的核心思路：
对于 INT/BIGINT（统一按 8 字节 int64 存储），把 int64 映射成无符号并翻转最高位
（即 uint64_t x = (uint64_t)v ^ (1ULL << 63)），再以大端序输出 8 字节。这样：

负数映射后始终小于 0 的正数映射；
同符号数字的大小关系保持不变；
8 字节定长且字典序 == 数值序。
*/
Key Value::to_key() const {
  return KeyCodecs::to_key(*this);
}

Value Value::from_key(const Key &key, DataType type) {
  return KeyCodecs::from_key(key, type);
}

// 类型最小值的 key
Key Value::min_key_for_type(DataType type) {
  return KeyCodecs::min_key_for_type(type);
}

Key Value::upper_key_for_type(DataType type) {
  return KeyCodecs::upper_key_for_type(type);
}


} // namespace sql