// byte_buffer.h
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace sql {

// ============================================================
// 长度前缀的字节缓冲读写
//
// 序列化格式 v1：
//   str  = [u32 len][len 字节内容]        (len 使用大端序)
//   u8   = [1 字节]
//   u32  = [4 字节大端]
//
// 之所以不用 '|' / ',' / ':' 之类的分隔符：key 编码是二进制，
// 字符串内容里可能包含任何字节，用分隔符拼接必然被内容击穿。
// ============================================================

class ByteWriter {
 public:
  void put_u8(uint8_t v) { buf_.push_back(static_cast<char>(v)); }

  void put_u32(uint32_t v) {
    for (size_t i = 0; i < 4; ++i) {
      buf_.push_back(static_cast<char>((v >> (24 - 8 * i)) & 0xFF));
    }
  }

  void put_str(std::string_view s) {
    put_u32(static_cast<uint32_t>(s.size()));
    buf_.append(s.data(), s.size());
  }

  const std::string& str() const { return buf_; }
  std::string take() { return std::move(buf_); }
  size_t size() const { return buf_.size(); }

 private:
  std::string buf_;
};

// 读失败时 status() 变为 false，后续读取都返回默认值，
// 因此调用方可以连续读、最后统一检查一次 status()。
class ByteReader {
 public:
  explicit ByteReader(std::string_view data) : data_(data) {}

  bool status() const { return ok_; }
  bool eof() const { return pos_ == data_.size(); }
  size_t remaining() const { return data_.size() - pos_; }

  uint8_t get_u8() {
    if (!ok_ || remaining() < 1) {
      ok_ = false;
      return 0;
    }
    return static_cast<uint8_t>(data_[pos_++]);
  }

  uint32_t get_u32() {
    if (!ok_ || remaining() < 4) {
      ok_ = false;
      return 0;
    }
    uint32_t v = 0;
    for (size_t i = 0; i < 4; ++i) {
      v |= static_cast<uint32_t>(static_cast<uint8_t>(data_[pos_ + i]))
           << (24 - 8 * i);
    }
    pos_ += 4;
    return v;
  }

  std::string get_str() {
    const uint32_t len = get_u32();
    if (!ok_ || remaining() < len) {
      ok_ = false;
      return {};
    }
    std::string out(data_.substr(pos_, len));
    pos_ += len;
    return out;
  }

 private:
  std::string_view data_;
  size_t pos_ = 0;
  bool ok_ = true;
};

}  // namespace sql
