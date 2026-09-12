// table.cpp
#include "table.h"

#include <algorithm>
#include <string>
#include <utility>

#include "cursor.h"
#include "key_prefix.h"
#include "storage/kv_engine/kv_engine.h"

namespace sql {
namespace {

RelError kv_error(const char *what, const kv::Status &status) {
  return RelError(RelErrorCode::KV_ERROR, std::string(what) + " failed: " +
                                              kv::status_to_string(status));
}

RelError schema_error(SchemaError err, const std::string &what) {
  return RelError(RelErrorCode::SCHEMA_ERROR,
                  what + ": " + TableSchema::error_message(err));
}

} // namespace

Table::Table(std::shared_ptr<kv::KVEngine> engine, Identifier db,
             TableSchema schema)
    : engine_(std::move(engine)), db_name_(std::move(db)),
      schema_(std::move(schema)) {
  key_prefix_ = keys::data_prefix(db_name_, schema_.table_name());
  const ColumnDef *pk = primary_key_column();
  primary_key_type_ = pk != nullptr ? pk->type : DataType::UNKNOWN_TYPE;
}

const ColumnDef *Table::primary_key_column() const {
  return schema_.has_primary_key() ? schema_.primary_key_column() : nullptr;
}

DataType Table::primary_key_type() const { return primary_key_type_; }

// ============================================================
// key / value 编解码
// ============================================================
std::string Table::encode_key(const Value &primary_key) const {
  return keys::data_key(db_name_, schema_.table_name(), primary_key,
                        primary_key_type_);
}

std::string Table::encode_row(const Row &row) const {
  return row.serialize(schema_);
}

std::expected<Row, RelError> Table::decode_row(std::string_view data) const {
  auto decoded = Row::deserialize(std::string(data), schema_);
  if (!decoded.has_value()) {
    return std::unexpected(schema_error(
        decoded.error(), "decode row of " + schema_.table_name().str()));
  }
  return std::move(*decoded);
}

Value Table::primary_key_of(const Row &row) const {
  const int index = schema_.primary_key_index();
  if (index < 0 || static_cast<size_t>(index) >= row.size()) {
    return Value();
  }
  return row[static_cast<size_t>(index)];
}

// ============================================================
// 点查
// ============================================================
std::expected<std::optional<Row>, RelError>
Table::find(const Value &primary_key) const {
  if (primary_key.is_null()) {
    return std::unexpected(
        RelError(RelErrorCode::PRIMARY_KEY_NULL, "primary key is NULL"));
  }
  std::string data;
  const kv::Status status = engine_->get(encode_key(primary_key), &data);
  if (status == kv::Status::NotFound) {
    return std::optional<Row>(std::nullopt);
  }
  if (status != kv::Status::OK) {
    return std::unexpected(kv_error("get", status));
  }
  auto decoded = decode_row(data);
  if (!decoded.has_value()) {
    return std::unexpected(decoded.error());
  }
  return std::optional<Row>(std::move(*decoded));
}

std::expected<Row, RelError> Table::get(const Value &primary_key) const {
  auto found = find(primary_key);
  if (!found.has_value()) {
    return std::unexpected(found.error());
  }
  if (!found->has_value()) {
    return std::unexpected(
        RelError(RelErrorCode::NOT_FOUND,
                 "row not found in " + schema_.table_name().str()));
  }
  return std::move(**found);
}

// ============================================================
// 扫描
// ============================================================
std::string Table::physical_start(const KeyRange &range) const {
  const StrKeyRange flat = range.to_str_key_range(primary_key_type_);
  return key_prefix_ + flat.start;
}

std::string Table::physical_end(const KeyRange &range) const {
  const StrKeyRange flat = range.to_str_key_range(primary_key_type_);
  return key_prefix_ + flat.end;
}

std::unique_ptr<TableCursor> Table::scan(const KeyRange &range,
                                         bool ascending) const {
  kv::KeyRange kv_range;
  kv_range.start = physical_start(range);
  kv_range.end =
      physical_end(range); // 半开：空集时 start == end，迭代器自然为空
  kv_range.direction =
      ascending ? kv::ScanDirection::kForward : kv::ScanDirection::kReverse;
  return std::make_unique<TableCursor>(this, range,
                                       engine_->new_iterator(kv_range));
}

std::unique_ptr<TableCursor> Table::scan_all(bool ascending) const {
  return scan(KeyRange::all(primary_key_type_), ascending);
}

std::expected<size_t, RelError> Table::row_count() const {
  size_t count = 0;
  auto cursor = scan_all();
  while (true) {
    auto row = cursor->next();
    if (row.has_value()) {
      ++count;
      continue;
    }
    if (row.error().end()) {
      break;
    }
    const auto code = row.error().code == CursorErrorCode::SCHEMA_ERROR
                          ? RelErrorCode::SCHEMA_ERROR
                          : RelErrorCode::KV_ERROR;
    return std::unexpected(RelError(code, row.error().to_string()));
  }
  return count;
}

// ============================================================
// 写
// ============================================================
std::expected<void, RelError> Table::insert(const Row &row) {
  const SchemaError valid = schema_.validate_row(row);
  if (valid != SchemaError::OK) {
    return std::unexpected(
        schema_error(valid, "insert into " + schema_.table_name().str()));
  }
  const Value pk = primary_key_of(row);
  if (pk.is_null()) {
    return std::unexpected(
        RelError(RelErrorCode::PRIMARY_KEY_NULL, "insert with NULL key"));
  }

  // 整行一个 blob（值 = Row::serialize(schema)）
  const kv::Status status = engine_->put(encode_key(pk), encode_row(row));
  if (status != kv::Status::OK) {
    return std::unexpected(kv_error("put", status));
  }
  return {};
}

std::expected<void, RelError> Table::update(const Value &primary_key,
                                            const Row &row) {
  if (primary_key.is_null()) {
    return std::unexpected(
        RelError(RelErrorCode::PRIMARY_KEY_NULL, "update with NULL key"));
  }
  const SchemaError valid = schema_.validate_row(row);
  if (valid != SchemaError::OK) {
    return std::unexpected(
        schema_error(valid, "update " + schema_.table_name().str()));
  }
  // 行里的主键必须和要更新的 key 一致，否则等于"搬动行"
  const Value row_pk = primary_key_of(row);
  if (!(row_pk == primary_key)) {
    return std::unexpected(
        RelError(RelErrorCode::PRIMARY_KEY_MISMATCH,
                 "primary key in row (" + row_pk.to_string() +
                     ") != target key (" + primary_key.to_string() + ")"));
  }
  const kv::Status status =
      engine_->put(encode_key(primary_key), encode_row(row));
  if (status != kv::Status::OK) {
    return std::unexpected(kv_error("put", status));
  }
  return {};
}

std::expected<void, RelError>
Table::update(const Value &primary_key,
              const std::vector<std::pair<Identifier, Value>> &assignments) {
  auto current = get(primary_key);
  if (!current.has_value()) {
    return std::unexpected(current.error());
  }
  Row updated = std::move(*current);
  for (const auto &[column, value] : assignments) {
    const int index = schema_.column_index(column);
    if (index < 0) {
      return std::unexpected(RelError(RelErrorCode::COLUMN_NOT_FOUND,
                                      "column not found: " + column.str()));
    }
    updated[static_cast<size_t>(index)] = value;
  }
  return update(primary_key, updated);
}

std::expected<void, RelError> Table::remove(const Value &primary_key) {
  if (primary_key.is_null()) {
    return std::unexpected(
        RelError(RelErrorCode::PRIMARY_KEY_NULL, "remove with NULL key"));
  }
  const kv::Status status = engine_->remove(encode_key(primary_key));
  if (status != kv::Status::OK && status != kv::Status::NotFound) {
    return std::unexpected(kv_error("remove", status));
  }
  return {};
}

std::expected<void, RelError> Table::truncate() {
  // 先把本表所有 key 收集出来再批量删（边遍历边删会动到迭代器状态）
  std::vector<std::string> keys;
  kv::KeyRange kv_range;
  kv_range.start = key_prefix_;
  kv_range.end = keys::prefix_end(key_prefix_);
  auto it = engine_->new_iterator(kv_range);
  while (it != nullptr && it->valid()) {
    keys.push_back(it->key());
    it->next();
  }
  if (it != nullptr && it->status() != kv::Status::OK &&
      it->status() != kv::Status::NotFound) {
    return std::unexpected(
        RelError(RelErrorCode::KV_ERROR,
                 "truncate scan failed: " + it->error_message()));
  }
  if (keys.empty()) {
    return {};
  }
  const kv::Status status = engine_->remove_batch(keys);
  if (status != kv::Status::OK) {
    return std::unexpected(kv_error("remove_batch", status));
  }
  return {};
}

} // namespace sql
