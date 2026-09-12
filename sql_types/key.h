#pragma once

#include <cstdint>
#include <string>
#include <string_view>

#include "field_type.h"
#include "value.h"

namespace sql {

// Key 类型别名只在 value.h 中定义一次：using Key = std::string;

// ============================================================
// Key 编码格式 v3（存储层契约）
//
//   key = [1 字节 family tag][1 字节 null flag][payload]
//     null flag: 0x00 = NULL, 0x01 = 有值
//
// family tag（是"编码族"标记，不等于 DataType 枚举值）：
//   kTagInt64  = 0x01 : TINYINT/SMALLINT/INT/BIGINT/DATE/TIME/DATETIME
//                       （底层都是 int64_t）
//                       payload = 8 字节大端 (uint64)v ^ (1<<63)
//   kTagString = 0x02 : VARCHAR/TEXT
//                       payload = 转义后的字节 + 0x00
//                       （0x00 -> 0x00 0xFF，结尾 0x00 终止符）
//   kTagBool   = 0x03 : BOOLEAN，payload = 0x00 / 0x01
//   kTagNull   = 0x00 : 无列类型信息时的 NULL（例如 KeySet 排序）；
//                       存储层请用带 column_type 的重载，NULL 会写成
//                       [本族 tag] 0x00
//
// 关键不变量（有测试保证）：
//   1) 全序：任意两个 key 都能比出大小（字节序），NULL 永远最小。
//   2) 稳定：相同的值 + 相同的列类型 -> 相同的 key。
//   3) 同族内：值的逻辑序 == key 的字节序（字符串是字典序，不是长度序）。
//   4) INT 系列与 DATE/TIME/DATETIME 编码相同（都是 int64）。
//   5) 族的上下界 = [tag] 0x00 与 [tag+1]，所以整族的 NULL + 值恰好被
//      [min_key_for_type, upper_key_for_type) 覆盖，不会串到别的族。
// ============================================================
class KeyCodecs {
public:
  static constexpr uint8_t kTagNull = 0x00;
  static constexpr uint8_t kTagInt64 = 0x01;
  static constexpr uint8_t kTagString = 0x02;
  static constexpr uint8_t kTagBool = 0x03;
  static constexpr uint8_t kNullFlag = 0x00;
  static constexpr uint8_t kValueFlag = 0x01;
  static constexpr uint8_t kFormatVersion = 3;

  KeyCodecs() = default;

  // 该逻辑类型使用的族标记；未知类型返回 kTagNull
  static uint8_t tag_of(DataType type) {
    if (is_integer(type) || is_temporal(type)) {
      return kTagInt64;
    }
    if (is_string(type)) {
      return kTagString;
    }
    if (is_boolean(type)) {
      return kTagBool;
    }
    return kTagNull;
  }

  // ---- 编码 ----

  // 不带列类型：NULL 用 kTagNull 编码（KeySet 排序等逻辑层用途）
  static Key to_key(const Value &val) {
    if (val.is_null()) {
      return make_null_key(kTagNull);
    }
    return encode_value(val, tag_of(val.type()));
  }

  // 带列类型：存储层用这个（NULL 会用本族的 tag 编码）
  static Key to_key(const Value &val, DataType column_type) {
    if (val.is_null()) {
      return make_null_key(tag_of(column_type));
    }
    // 非 NULL 值按自己的 family 编码；调用方应先用 schema 校验值类型
    return encode_value(val, tag_of(val.type()));
  }

  // ---- 解码 ----

  // 解析失败返回 Value()（NULL）；可用 is_valid_key 先判定
  static Value from_key(const Key &key, DataType type) {
    if (key.empty()) {
      return Value();
    }
    const uint8_t tag = static_cast<uint8_t>(key[0]);

    // 无类型信息的 NULL key
    if (tag == kTagNull) {
      return Value();
    }
    if (tag != tag_of(type) || key.size() < 2) {
      return Value();
    }
    if (static_cast<uint8_t>(key[1]) == kNullFlag) {
      return Value(); // 本族的 NULL
    }
    if (static_cast<uint8_t>(key[1]) != kValueFlag) {
      return Value();
    }
    return decode_value(key, tag, type);
  }

  static bool is_valid_key(const Key &key, DataType type) {
    if (key.empty()) {
      return false;
    }
    const uint8_t tag = static_cast<uint8_t>(key[0]);
    if (tag == kTagNull) {
      return key.size() == 2; // 无类型 NULL
    }
    if (tag != tag_of(type) || key.size() < 2) {
      return false;
    }
    const uint8_t flag = static_cast<uint8_t>(key[1]);
    if (flag == kNullFlag) {
      return key.size() == 2; // 本族 NULL
    }
    if (flag != kValueFlag) {
      return false;
    }
    switch (tag) {
    case kTagInt64:
      return key.size() == 2 + 8;
    case kTagBool:
      return key.size() == 2 + 1 && (key[2] == 0x00 || key[2] == 0x01);
    case kTagString: {
      std::string ignored;
      return decode_string(key, 2, ignored);
    }
    default:
      return false;
    }
  }

  // 包含式上界：k 之后紧随的字节串（用于单点/闭区间的上界）。
  static Key inclusive_upper_bound(const Key &k) {
    Key out = k;
    out.push_back('\0');
    return out;
  }

  // 指定类型族的全局上下界（左闭右开）
  //   下界 = [tag] 0x00，正好等于本族的 NULL key -> 扫描会包含 NULL
  //   上界 = [tag+1]
  static Key min_key_for_type(DataType type) {
    const uint8_t tag = tag_of(type);
    return make_null_key(tag == kTagNull ? kTagNull : tag);
  }

  static Key upper_key_for_type(DataType type) {
    const uint8_t tag = tag_of(type);
    if (tag == kTagNull) {
      // 未知类型：覆盖所有族，["", 0xFF)
      return Key{static_cast<char>(0xFF)};
    }
    return Key{static_cast<char>(tag + 1)};
  }

  // 本族的 NULL key（= 族下界）
  static Key null_key_for_type(DataType type) { return min_key_for_type(type); }

  // 非 NULL 值的"前缀 key"：[tag][VALUE_FLAG]
  //
  // 它不是任何值产生的完整 key（值 key 至少 3 字节），但它是**所有非 NULL 值
  // key 的前缀**，可以当作"第一个非 NULL 值"的包含式下界，
  // 用来把 NULL 排除在扫描范围之外（见 KeyRange::non_null()）。
  static Key first_value_key(DataType type) {
    const uint8_t tag = tag_of(type);
    Key k;
    k.push_back(static_cast<char>(tag));
    k.push_back(static_cast<char>(kValueFlag));
    return k;
  }

  // ---- 全序比较（存储层语义）----
  // NULL 最小；其余按编码后 key 的字节序。

  static int compare(const Key &a, const Key &b) {
    if (a == b) {
      return 0;
    }
    return a < b ? -1 : 1;
  }

  static int compare(const Value &a, const Value &b) {
    return compare(a.to_key(), b.to_key());
  }

  static bool less(const Value &a, const Value &b) {
    return a.to_key() < b.to_key();
  }

  // 字符串 key 的最小后继（用于需要"严格大于前缀"的场景）
  static Key successor(const Key &k) {
    Key out = k;
    while (!out.empty() && static_cast<uint8_t>(out.back()) == 0xFF) {
      out.pop_back();
    }
    if (out.empty()) {
      return Key(); // 没有后继（视为 +∞）
    }
    out.back() = static_cast<char>(static_cast<uint8_t>(out.back()) + 1);
    return out;
  }

private:
  // [tag][0x00]：本族的 NULL key，也是本族的最小 key
  static Key make_null_key(uint8_t tag) {
    Key k;
    k.push_back(static_cast<char>(tag));
    k.push_back(static_cast<char>(kNullFlag));
    return k;
  }

  static Key encode_value(const Value &val, uint8_t tag) {
    Key k;
    k.reserve(16);
    k.push_back(static_cast<char>(tag));
    k.push_back(static_cast<char>(kValueFlag));

    switch (tag) {
    case kTagBool:
      k.push_back(val.as_bool() ? 0x01 : 0x00);
      break;
    case kTagInt64:
      encode_be64(int64_to_ordered(val.as_int()), k);
      break;
    case kTagString:
      encode_string(val.as_str(), k);
      break;
    default:
      return Key(); // 不可编码的类型
    }
    return k;
  }

  static Value decode_value(const Key &key, uint8_t tag, DataType type) {
    switch (tag) {
    case kTagBool: {
      if (key.size() != 3) {
        return Value();
      }
      return Value(key[2] != 0x00);
    }
    case kTagInt64: {
      if (key.size() != 10) {
        return Value();
      }
      const int64_t v = ordered_to_int64(decode_be64(key.data() + 2));
      // 时间类型/整型宽度都保留调用方要求的逻辑类型
      return Value(v, is_temporal(type) ? type : DataType::BIGINT);
    }
    case kTagString: {
      std::string out;
      if (!decode_string(key, 2, out)) {
        return Value();
      }
      return Value(std::move(out), is_string(type) ? type : DataType::VARCHAR);
    }
    default:
      return Value();
    }
  }

  // --- 大端序 ---
  static void encode_be64(uint64_t v, Key &out) {
    for (size_t i = 0; i < 8; ++i) {
      out.push_back(static_cast<char>((v >> (56 - 8 * i)) & 0xFF));
    }
  }

  static uint64_t decode_be64(const char *p) {
    uint64_t v = 0;
    for (size_t i = 0; i < 8; ++i) {
      v |= static_cast<uint64_t>(static_cast<uint8_t>(p[i])) << (56 - 8 * i);
    }
    return v;
  }

  // 有符号 -> 无符号保序映射
  static uint64_t int64_to_ordered(int64_t v) {
    return static_cast<uint64_t>(v) ^ 0x8000000000000000ull;
  }

  static int64_t ordered_to_int64(uint64_t v) {
    return static_cast<int64_t>(v ^ 0x8000000000000000ull);
  }

  // --- 字符串：0x00 -> 0x00 0xFF，结尾 0x00 ---
  static void encode_string(std::string_view s, Key &out) {
    out.reserve(out.size() + s.size() + 1);
    for (char c : s) {
      if (c == '\0') {
        out.push_back('\0');
        out.push_back(static_cast<char>(0xFF));
      } else {
        out.push_back(c);
      }
    }
    out.push_back('\0'); // 终止符
  }

  static bool decode_string(const Key &key, size_t pos, std::string &out) {
    out.clear();
    while (pos < key.size()) {
      const unsigned char c = static_cast<unsigned char>(key[pos]);
      if (c != 0x00) {
        out.push_back(static_cast<char>(c));
        ++pos;
        continue;
      }
      if (pos + 1 < key.size() &&
          static_cast<unsigned char>(key[pos + 1]) == 0xFF) {
        out.push_back('\0');
        pos += 2;
        continue;
      }
      return pos + 1 == key.size(); // 终止符必须正好在结尾
    }
    return false;
  }
};

} // namespace sql
