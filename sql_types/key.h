#pragma once


#include <string>
#include <cstdint>
#include <cstring>
#include "field_type.h"
#include "value.h"

namespace sql{

using Key = std::string;

class KeyCodecs {
    // 假设类内部有 type_、int_val_、str_val_、bool_val_ 等成员
public:
    KeyCodecs() = default;

    // 将 Value 编码为 Key（带类型标记）
    static Key to_key(const Value& val) {
        Key k;
        switch (val.type()) {
        case DataType::BOOLEAN:
            k.push_back(static_cast<char>(DataType::BOOLEAN));
            k.push_back(val.as_bool() ? 0x01 : 0x00);
            break;

        case DataType::INT: {
            k.push_back(static_cast<char>(DataType::INT));
            uint32_t uv = static_cast<uint32_t>(val.as_int()) + 0x80000000u;
            k.resize(5);                     // 1字节标记 + 4字节数据
            encode_be32(uv, k, 1);
            break;
        }

        case DataType::BIGINT: {
            k.push_back(static_cast<char>(DataType::BIGINT));
            uint64_t uv = static_cast<uint64_t>(val.as_int()) + 0x8000000000000000ull;
            k.resize(9);                     // 1字节标记 + 8字节数据
            encode_be64(uv, k, 1);
            break;
        }

        case DataType::VARCHAR:
        case DataType::TEXT: {
            k.push_back(static_cast<char>(val.type()));
            const auto& str = val.as_str();
            uint32_t len = static_cast<uint32_t>(str.size());
            k.resize(5);                     // 1字节标记 + 4字节长度
            encode_be32(len, k, 1);
            k += str;                   // 追加内容
            break;
        }

        default:
            break;
        }
        return k;
    }

    // 从 Key 解析 Value（Key 必须包含类型标记，type 用于校验和指定解析方式）
    static Value from_key(const Key& key, DataType type) {
        // 若 Key 为空或首字节不匹配，可根据策略处理（这里视为无效）
        if (key.empty() || key[0] != static_cast<char>(type)) {
            return Value(); // 返回空值
        }

        switch (type) {
        case DataType::INT: {
            if (key.size() < 1 + 4) { return Value();
}
            uint32_t uv = 0;
            for (size_t i = 0; i < 4; ++i) {
                uv |= (static_cast<uint32_t>(static_cast<uint8_t>(key[1 + i])) << (24 - 8 * i));
            }
            int32_t val = static_cast<int32_t>(uv - 0x80000000u);
            return Value(val);
        }

        case DataType::BIGINT: {
            if (key.size() < 1 + 8) { return Value();
}
            uint64_t uv = 0;
            for (size_t i = 0; i < 8; ++i) {
                uv |= (static_cast<uint64_t>(static_cast<uint8_t>(key[1 + i])) << (56 - 8 * i));
            }
            int64_t val = static_cast<int64_t>(uv - 0x8000000000000000ull);
            return Value(val);
        }

        case DataType::VARCHAR:
        case DataType::TEXT: {
            if (key.size() < 1 + 4) return Value();
            uint32_t len = 0;
            for (size_t i = 0; i < 4; ++i) {
                len |= (static_cast<uint32_t>(static_cast<uint8_t>(key[1 + i])) << (24 - 8 * i));
            }
            if (key.size() < 1 + 4 + len) { return Value(); // 长度不足
}
            std::string str = key.substr(1 + 4, len);
            return Value(str); // 根据类型构造，此处假设 Value 有相应构造函数
        }

        case DataType::BOOLEAN: {
            if (key.size() < 1 + 1) { return Value();
}
            return Value(key[1] != '\0');
        }

        default:
            return Value();
        }
    }

    // 返回指定类型的最小 Key（包含类型标记）
    static Key min_key_for_type(DataType type) {
        Key k;
        k.push_back(static_cast<char>(type));
        switch (type) {
        case DataType::INT:
            k.append(4, '\0');          // 4字节全0，对应最小值
            break;
        case DataType::BIGINT:
            k.append(8, '\0');          // 8字节全0，对应最小值
            break;
        case DataType::VARCHAR:
        case DataType::TEXT:
            k.append(4, '\0');          // 长度0，内容为空
            break;
        case DataType::BOOLEAN:
            k.push_back('\0');          // false
            break;
        default:
            break;
        }
        return k;
    }

    // 返回指定类型的最大 Key（包含类型标记，已提供，但可保持）
    static Key upper_key_for_type(DataType type) {
        Key k;
        k.push_back(static_cast<char>(type));
        switch (type) {
        case DataType::BOOLEAN:
            k.push_back(0xFF);
            break;
        case DataType::INT:
            k.append(4, 0xFF);
            break;
        case DataType::BIGINT:
            k.append(8, 0xFF);
            break;
        case DataType::VARCHAR:
        case DataType::TEXT:
            k.append(4, 0xFF);   // 长度最大
            k.append(16, 0xFF);  // 内容足够大，实际比较时长度优先
            break;
        default:
            break;
        }
        return k;
    }

private:
    // 辅助函数：大端序写入 32 位无符号整数
    static void encode_be32(uint32_t v, Key& out, size_t offset) {
        for (size_t i = 0; i < 4; ++i) {
            out[offset + i] = static_cast<char>((v >> (24 - 8 * i)) & 0xFF);
        }
    }

    // 辅助函数：大端序写入 64 位无符号整数
    static void encode_be64(uint64_t v, Key& out, size_t offset) {
        for (size_t i = 0; i < 8; ++i) {
            out[offset + i] = static_cast<char>((v >> (56 - 8 * i)) & 0xFF);
        }
    }

};

}