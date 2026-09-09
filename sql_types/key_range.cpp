#include "key_range.h"
#include "key_set.h"

namespace sql {

// 判断是否有任何点在此 range
bool KeyRange::intersects_set(const KeySet &keys) const {
  for (const auto &v : keys.points()) {
    if (contains(v)) {
      return true;
    }
  }
  return false;
}

// 取出范围内所有点
KeySet KeyRange::filter_set(const KeySet &keys) const {
  KeySet result;
  for (const auto &v : keys.points()) {
    if (contains(v)) {
      result.add(v);
    }
  }
  return result;
}
} // namespace sql