#pragma once


#include <cstdint>
#include <cstring>
#include <optional>
#include <string>
#include <string_view>

#include "field_type.h"
#include "value.h"

namespace sql{

// Key 类型别名只在 value.h 中定义一次：using Key = std::string;

// ============================================================
// Key 编码格式 v2
//
//   key = [1 字节 tag][payload]
//
// tag 是"编码族"标记，**不等于** DataType 的枚举值：
// 枚举值将来重排/扩充不会影响已落盘的 key 格式。
//
// 族标记：
//   kTagInt64  = 0x01 : INT / BIGINT 共用，payload = 8 字节
//                      (uint64)(v) ^ (1<<63)，大端；字典序 == 数值序
//   kTagString = 0x02 : VARCHAR / TEXT 共用，payload = 转义后的字节 + 0x00
//                      0x00 转义成 0x00 0xFF，结尾 0x00 作终止符
//   kTagBool   = 0x03 : payload = 0x00(false) / 0x01(true)
//
// NULL 不产生 key（返回空串），因为 NULL 不参与索引。
//
// 关键不变量（有测试保证）：
//   1) 同一族内，key 的字节序 == 值的逻辑序（字符串是字典序，不是长度序）。
//   2) Value::to_key() 往返到 from_key(key, type) 得到与逻辑类型一致的值
//      （TEXT 仍是 TEXT，BIGINT 仍是 BIGINT）。
//   3) INT 与 BIGINT 的编码完全相同 —— 与 Value::operator== 的族比较一致。
// ============================================================
class KeyCodecs {
public:
    static constexpr uint8_t kTagInt64  = 0x01;
    static constexpr uint8_t kTagString = 0x02;
    static constexpr uint8_t kTagBool   = 0x03;
    static constexpr uint8_t kFormatVersion = 2;

    KeyCodecs() = default;

    // 该逻辑类型使用的族标记；不支持的类型返回 0
    static uint8_t tag_of(DataType type) {
        if (is_integer(type)) {
            return kTagInt64;
        }
        if (is_string(type)) {
            return kTagString;
        }
        if (is_boolean(type)) {
            return kTagBool;
        }
        return 0;
    }

    // 将 Value 编码为 Key（带族标记）。NULL / 未知类型 -> 空 key。
    static Key to_key(const Value& val) {
        const uint8_t tag = tag_of(val.type());
        if (tag == 0) {
            return Key();          // NULL / UNKNOWN 不参与索引
        }

        Key k;
        k.reserve(16);
        k.push_back(static_cast<char>(tag));

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
            break;
        }
        return k;
    }

    // 从 Key 解析 Value；type 指定期望的逻辑类型（INT/BIGINT/VARCHAR/
    // TEXT/BOOLEAN 均可，解码后保留 type 本身）。
    // 解析失败返回 Value()（NULL），调用方可用 is_valid_key 先判定。
    static Value from_key(const Key& key, DataType type) {
        const uint8_t tag = tag_of(type);
        if (tag == 0 || key.empty() ||
            static_cast<uint8_t>(key[0]) != tag) {
            return Value();
        }

        switch (tag) {
        case kTagBool: {
            if (key.size() != 2) {
                return Value();
            }
            return Value(key[1] != 0x00);
        }

        case kTagInt64: {
            if (key.size() != 9) {
                return Value();
            }
            return Value(ordered_to_int64(decode_be64(key.data() + 1)));
        }

        case kTagString: {
            std::string out;
            if (!decode_string(key, 1, out)) {
                return Value();
            }
            // 保留位宽/长度语义：TEXT 解出来仍是 TEXT
            return Value(std::move(out), type);
        }

        default:
            return Value();
        }
    }

    // key 是否是 type 对应的合法编码
    static bool is_valid_key(const Key& key, DataType type) {
        const uint8_t tag = tag_of(type);
        if (tag == 0 || key.empty() ||
            static_cast<uint8_t>(key[0]) != tag) {
            return false;
        }
        switch (tag) {
        case kTagBool:
            return key.size() == 2;
        case kTagInt64:
            return key.size() == 9;
        case kTagString: {
            std::string ignored;
            return decode_string(key, 1, ignored);
        }
        default:
            return false;
        }
    }

    // 包含式上界：k 之后紧随的字节串。
    // 因为字符串编码以 0x00 结尾、转义序列以 0x00 0xFF 开头，
    // 追加一个 0x00 恰好是"大于等于 k"的最小可用上界，
    // 且不会包含任何以 k 为前缀的其他值。
    static Key inclusive_upper_bound(const Key& k) {
        Key out = k;
        out.push_back('\0');
        return out;
    }

    // 指定类型的下界 Key：就是族标记本身（它是所有族内 key 的前缀）。
    // 因为 "前缀 < 任何带后缀的串"，[tag] 一定 <= 族内所有 key。
    static Key min_key_for_type(DataType type) {
        Key k;
        const uint8_t tag = tag_of(type);
        if (tag == 0) {
            return k;                     // 未知类型：-∞
        }
        k.push_back(static_cast<char>(tag));
        return k;
    }

    // 指定类型的上界 Key（不含）：族标记 +1。
    // 族标记是连续的小整数，所以 [tag] 与 [tag+1] 之间恰好只有本族的
    // key；这比"用 0xFF 填充的伪上界"更严格，也不会被含 0xFF 字节的
    // 字符串击穿。
    static Key upper_key_for_type(DataType type) {
        Key k;
        const uint8_t tag = tag_of(type);
        if (tag == 0) {
            // 未知类型：覆盖所有族，["", 0xFF)
            k.push_back(static_cast<char>(0xFF));
            return k;
        }
        k.push_back(static_cast<char>(tag + 1));
        return k;
    }

private:
    // --- 大端序 ---
    static void encode_be64(uint64_t v, Key& out) {
        for (size_t i = 0; i < 8; ++i) {
            out.push_back(static_cast<char>((v >> (56 - 8 * i)) & 0xFF));
        }
    }

    static uint64_t decode_be64(const char* p) {
        uint64_t v = 0;
        for (size_t i = 0; i < 8; ++i) {
            v |= static_cast<uint64_t>(static_cast<uint8_t>(p[i]))
                 << (56 - 8 * i);
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
    static void encode_string(std::string_view s, Key& out) {
        out.reserve(out.size() + s.size() + 1);
        for (char c : s) {
            if (c == '\0') {
                out.push_back('\0');
                out.push_back(static_cast<char>(0xFF));
            } else {
                out.push_back(c);
            }
        }
        out.push_back('\0');   // 终止符
    }

    // 从 key[pos] 解出字符串；成功返回 true 且 out 为原始字节
    static bool decode_string(const Key& key, size_t pos, std::string& out) {
        out.clear();
        while (pos < key.size()) {
            const unsigned char c = static_cast<unsigned char>(key[pos]);
            if (c != 0x00) {
                out.push_back(static_cast<char>(c));
                ++pos;
                continue;
            }
            // c == 0x00：要么是转义的 0x00，要么是终止符
            if (pos + 1 < key.size() &&
                static_cast<unsigned char>(key[pos + 1]) == 0xFF) {
                out.push_back('\0');
                pos += 2;
                continue;
            }
            // 终止符：必须正好在结尾
            return pos + 1 == key.size();
        }
        return false;   // 没有终止符
    }
};

}
