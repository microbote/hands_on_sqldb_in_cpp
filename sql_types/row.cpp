// row.cpp
#include "row.h"
#include "schema.h"

#include "byte_buffer.h"
#include "identifier.h"
#include "key.h"
#include <sstream>

namespace sql {

std::string Row::to_string() const {
  std::ostringstream oss;
  oss << "[";
  for (size_t i = 0; i < values_.size(); ++i) {
    if (i > 0) { oss << ", ";
}
    oss << values_[i].to_string();
  }
  oss << "]";
  return oss.str();
}

// ============================================================
// 序列化（格式 v1：长度前缀 framing）
//
//   [u8 version][u32 field_count]{ [str field] }*
//   field = 该列的 key 编码；长度 0 表示 NULL
//
// 不能用 '|' 拼接 key：key 编码里含原始字符串字节，值里一旦出现
// '|'(0x7C) 就会被切错（旧实现的确会）。
// ============================================================

std::string Row::serialize() const {
  ByteWriter w;
  w.put_u8(kFormatVersion);
  w.put_u32(static_cast<uint32_t>(values_.size()));
  for (const auto& v : values_) {
    w.put_str(v.to_key());   // NULL -> 空串 -> 长度为 0
  }
  return w.take();
}

std::expected<Row, SchemaError> Row::deserialize(const std::string& data,
                                                 const TableSchema& schema) {
  ByteReader r(data);

  const uint8_t version = r.get_u8();
  if (!r.status() || version != kFormatVersion) {
    return std::unexpected(SchemaError::INVALID_FORMAT);
  }

  const uint32_t field_count = r.get_u32();
  if (!r.status() || field_count != schema.columns().size()) {
    return std::unexpected(SchemaError::COLUMN_SIZE_MISMATCH);
  }

  Row row;
  row.reserve(field_count);
  for (uint32_t i = 0; i < field_count; ++i) {
    const std::string field = r.get_str();
    if (!r.status()) {
      return std::unexpected(SchemaError::INVALID_FORMAT);
    }
    const auto& col = schema.columns()[i];
    if (field.empty()) {
      row.push_back(Value());          // NULL
      continue;
    }
    if (!KeyCodecs::is_valid_key(field, col.type)) {
      return std::unexpected(SchemaError::INVALID_FORMAT);
    }
    row.push_back(Value::from_key(field, col.type));
  }

  if (!r.eof()) {
    return std::unexpected(SchemaError::INVALID_FORMAT);
  }

  return row;
}


// ============================================================
// RowBuilder 实现
// ============================================================

std::expected<Row, SchemaError> RowBuilder::build(
    const TableSchema& schema) const {
  // 0. schema 之外的列一律拒绝：拼错列名必须报错，
  //    否则 INSERT 会静默少写数据（旧实现就是这样）。
  for (const auto& [name, value] : values_) {
    if (schema.column(name) == nullptr) {
      return std::unexpected(SchemaError::COLUMN_NOT_FOUND);
    }
  }

  Row row;
  row.reserve(schema.columns().size());

  for (const auto& col : schema.columns()) {
    auto it = values_.find(col.name);
    if (it != values_.end()) {
      row.push_back(it->second);
    } else {
      // 缺少 NOT NULL 列：整行无效
      if (!col.nullable) {
        return std::unexpected(SchemaError::INVALID_ROW);
      }
      row.push_back(Value());  // NULL
    }
  }

  // 验证行（类型族检查、NULL 约束等）
  auto err = schema.validate_row(row);
  if (err != SchemaError::OK) {
    return std::unexpected(err);
  }

  return row;
}

std::expected<Row, SchemaError> RowBuilder::build_ordered(
    const TableSchema& schema, const std::vector<Identifier>& order) const {
  Row row;
  row.reserve(order.size());

  for (const auto& col_name : order) {
    const auto* col = schema.column(col_name);
    if (!col) {
      return std::unexpected(SchemaError::COLUMN_NOT_FOUND);
    }
    auto it = values_.find(col_name);
    if (it != values_.end()) {
      // 按"列定义"校验，而不是按 schema 位置校验：
      // 本函数的语义就是允许行的顺序与 schema 不同。
      auto err = schema.validate_value(*col, it->second);
      if (err != SchemaError::OK) {
        return std::unexpected(err);
      }
      row.push_back(it->second);
    } else {
      if (!col->nullable) {
        return std::unexpected(SchemaError::INVALID_ROW);
      }
      row.push_back(Value());
    }
  }

  return row;
}

Value RowBuilder::get(const Identifier& column) const {
  auto it = values_.find(column);
  return it != values_.end() ? it->second : Value();
}

bool RowBuilder::has(const Identifier& column) const {
  return values_.find(column) != values_.end();
}

}  // namespace sql
