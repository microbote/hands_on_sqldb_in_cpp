  #pragma once

  #include "sql_types/key_range.h"
  #include "storage/kv_engine/kv_engine.h"
  // ============================================================
  // 转换为 KV 引擎的 KeyRange
  // ============================================================
  inline std::optional<kv::KeyRange> to_kv_range(const sql::KeyRange& sqr)  {
    kv::KeyRange kvr;
    auto & k = sqr;
    if (k.is_empty()) {
      // 空集无法表示，返回无效范围
      return std::nullopt;
    } else if (k.is_all()) {
      return {};
    } else if (k.is_bounded()) {
      if (k.has_start()) {
        kvr.start = k.start().to_string();
      }
      if (has_end()) {
        kvr.end = k.end().to_string();
      }
    }
    return kvr;
  }