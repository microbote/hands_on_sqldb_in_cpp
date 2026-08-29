// table.cpp
#include "table.h"

#include <iostream>
#include <sstream>
#include "key_prefix.h"
#include "cursor.h"

namespace sql {

Table::Table(std::shared_ptr<kv::KVEngine> engine, const std::string& db_name,
             const TableSchema& schema)
    : engine_(engine), db_name_(db_name), schema_(schema) {
  key_prefix_ = keys::data_prefix(db_name_, schema_.name());
}

// ============================================================
// 插入操作
// ============================================================

bool Table::insert(const Row& row) {
  auto err = schema_.validate_row(row);
  if (SchemaError::OK != err) {
    std::cerr << "❌ Insert failed: invalid row for table " << schema_.name()
              << ": " << TableSchema::error_message(err)
              << std::endl;
    return false;
  }

  Value pk = get_primary_key(row);
  if (pk.is_null()) {
    std::cerr << "❌ Insert failed: null primary key" << std::endl;
    return false;
  }

  std::string key = this->to_kv_key(pk);
  std::string value = encode_row(row);

  return engine_->put(key, value) == kv::Status::OK;
}

bool Table::insert(const std::vector<Value>& values) {
  return insert(Row(values));
}

bool Table::insert_batch(const std::vector<Row>& rows) {
  kv::WriteBatch batch;

  for (const auto& row : rows) {
    auto err = schema_.validate_row(row);
    if (SchemaError::OK != err) {
      std::cerr << "❌ Batch insert failed: invalid row. Error: "
                << TableSchema::error_message(err) << std::endl;
      return false;
    }

    Value pk = get_primary_key(row);
    if (pk.is_null()) {
      std::cerr << "❌ Batch insert failed: null primary key:"
                << row.to_string() << std::endl;
      return false;
    }

    std::string key = to_kv_key(pk);
    std::string value = encode_row(row);
    batch.put(key, value);
  }

  return engine_->write_batch(batch) == kv::Status::OK;
}

// ============================================================
// 查询操作
// ============================================================

std::optional<Row> Table::get(const Value& primary_key) const{
  if (primary_key.is_null()) {
    return std::nullopt;
  }

  std::string key = to_kv_key(primary_key);
  std::string value;
  kv::Status status = engine_->get(key, &value);

  if (status != kv::Status::OK) {
    return std::nullopt;
  }

  return decode_row(value);
}

std::optional<Row> Table::get(const std::string& primary_key) const {
  return get(Value(primary_key));
}

std::optional<Row> Table::get(int64_t primary_key) const {
  return get(Value(primary_key));
}

// 批量点查询
bool Table::get_batch(const std::vector<Value>& primary_keys,
               TableMissingKeyPolicy policy,
               std::vector<std::optional<Row>>* values) const {
    kv::MissingKeyPolicy kv_policy;

    switch (policy) {
      case TableMissingKeyPolicy::kReturnError:
        kv_policy = kv::MissingKeyPolicy::kReturnError;
        break;
      case TableMissingKeyPolicy::kReturnEmpty:
        kv_policy = kv::MissingKeyPolicy::kReturnEmpty;
        break;
    }
    std::vector<std::optional<kv::ByteValue>> kv_values;
    std::vector<std::string> kv_keys(primary_keys.size());
    for(int i=0;i<primary_keys.size();i++){
      kv_keys[i] = to_kv_key(primary_keys[i]);
    }
    auto status = engine_->get_batch(kv_keys, kv_policy, &kv_values);

    for (size_t i = 0; i < kv_values.size(); ++i) {
      if (kv_values[i].has_value()) {
        values->push_back(decode_row(*kv_values[i]));
      } else {
        values->push_back(std::nullopt);
      }
    }
    if(status != kv::Status::OK){
      fprintf(stderr, "get_batch error, batchsize:%d, from:%s\n",
              kv_keys.size(), get_key_prefix().c_str());
      return false;
    }
    return true;
}

// ============================================================
// 更新操作
// ============================================================

bool Table::update(const Value& primary_key, const Row& row) {
  if (primary_key.is_null()) {
    return false;
  }

  if (SchemaError::OK != schema_.validate_row(row)) {
    std::cerr << "❌ Update failed: invalid row" << std::endl;
    return false;
  }

  // 检查主键是否匹配
  Value row_pk = get_primary_key(row);
  if (row_pk != primary_key) {
    std::cerr << "❌ Update failed: primary key mismatch" << std::endl;
    return false;
  }

  std::string key = to_kv_key(primary_key);
  std::string value = encode_row(row);

  return engine_->put(key, value) == kv::Status::OK;
}

bool Table::update(
    const Value& primary_key,
    const std::vector<std::pair<std::string, Value>>& assignments) {
  auto existing = get(primary_key);
  if (!existing) {
    return false;
  }

  Row updated = *existing;
  for (const auto& [col_name, val] : assignments) {
    int idx = schema_.column_index(col_name);
    if (idx < 0) {
      std::cerr << "❌ Update failed: column '" << col_name << "' not found"
                << std::endl;
      return false;
    }
    updated[idx] = val;
  }

  return update(primary_key, updated);
}

// ============================================================
// 删除操作
// ============================================================

bool Table::remove(const Value& primary_key) {
  if (primary_key.is_null()) {
    return false;
  }

  std::string key = to_kv_key(primary_key);
  return engine_->remove(key) == kv::Status::OK;
}

bool Table::remove(const std::string& primary_key) {
  return remove(Value(primary_key));
}

bool Table::remove(int64_t primary_key) { return remove(Value(primary_key)); }

// ============================================================
// 扫描操作
// ============================================================
kv::KeyRange Table::to_kv_range(const PrimaryKeyRange& range) const {
  kv::KeyRange kv_range;
  kv_range.direction = (range.direction == ScanDirection::kForward)
                           ? kv::ScanDirection::kForward
                           : kv::ScanDirection::kReverse;
  kv_range.limit = range.limit;

  if (range.start) {
    kv_range.start = to_kv_key(*range.start);
  }else{
    kv_range.start = key_prefix_;
  }
  if (range.end) {
    kv_range.end = to_kv_key(*range.end);
  }else{
    kv_range.end = key_prefix_ + '\xFF';
  }

  return kv_range;
}


std::unique_ptr<Cursor> Table::scan(const PrimaryKeyRange& range) {
  // 由 Table 创建 Cursor，传入 this 指针
  if(!engine_){
    fprintf(stderr, "engine is null from %s\n", key_prefix_.c_str());
    return nullptr;
  }
  auto kv_iter = engine_->new_iterator(to_kv_range(range));
  if (!kv_iter)
  {
    fprintf(stderr, "scan iterator is null from %s\n", key_prefix_.c_str());
    return nullptr;
  }

  return std::make_unique<TableCursor>(this, range, std::move(kv_iter));
}

std::unique_ptr<Cursor> Table::scan_all()
{
  return scan(PrimaryKeyRange::all());
}

// ============================================================
// 统计和清空
// ============================================================

size_t Table::row_count() const {
  size_t count = 0;
  kv::KeyRange range = kv::KeyRange::prefix(key_prefix_);
  auto it = engine_->new_iterator(range);

  if (!it) {
    return 0;
  }

  while (it->valid()) {
    count++;
    it->next();
  }

  return count;
}

bool Table::truncate() {
  // 简单实现：删除所有数据
  // 注意：对于大量数据，应该分批删除
  kv::KeyRange range = kv::KeyRange::prefix(key_prefix_);
  auto it = engine_->new_iterator(range);

  if (!it) {
    return false;
  }

  kv::WriteBatch batch;
  size_t count = 0;
  const size_t BATCH_SIZE = 1000;

  while (it->valid()) {
    batch.remove(it->key());
    count++;

    if (count >= BATCH_SIZE) {
      engine_->write_batch(batch);
      batch.clear();
      count = 0;
    }
    it->next();
  }

  if (count > 0) {
    engine_->write_batch(batch);
  }

  return true;
}

// ============================================================
// 辅助方法
// ============================================================

Value Table::get_primary_key(const Row& row) const {
  if (schema_.primary_key_index() < 0) {
    return Value();
  }
  int idx = schema_.primary_key_index();
  if (idx >= static_cast<int>(row.size())) {
    return Value();
  }
  return row[idx];
}

std::string Table::encode_row(const Row& row) const { return row.serialize(); }

Row Table::decode_row(const std::string& data) const {
  return Row::deserialize(data, schema_);
}

}  // namespace sql