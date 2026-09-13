// kv_catalog.cpp
#include "kv_catalog.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "key_prefix.h"
#include "storage/kv_engine/kv_engine.h"

namespace sql {
namespace {

// 名单的序列化格式（带版本号，格式变化时递增）：
//   [1 字节版本=1][重复: 4 字节 LE 长度 + 名字字节]
// 用长度前缀而不是逗号拼接：名字里可能有分隔符，拼字符串会串行。
constexpr uint8_t kNameListVersion = 1;

void append_u32_le(std::string &out, uint32_t value) {
  for (int i = 0; i < 4; ++i) {
    out.push_back(static_cast<char>((value >> (8 * i)) & 0xFF));
  }
}

bool read_u32_le(const std::string &data, size_t &pos, uint32_t &out) {
  if (pos + 4 > data.size()) {
    return false;
  }
  out = 0;
  for (int i = 0; i < 4; ++i) {
    out |= static_cast<uint32_t>(
               static_cast<unsigned char>(data[pos + static_cast<size_t>(i)]))
           << (8 * i);
  }
  pos += 4;
  return true;
}

// 统计记录（固定长度 + 版本号）：
//   v1（17 字节）：[版本=1][8B created][8B last_write]              —— 旧数据
//   v2（25 字节）：[版本=2][8B created][8B last_write][8B rows]
// rows = -1 表示未知（旧记录 / 还没统计过）。
constexpr uint8_t kStatsVersion = 2;

std::string encode_stats(int64_t created_at, int64_t last_write_at,
                         int64_t row_count) {
  std::string out;
  out.reserve(25);
  out.push_back(static_cast<char>(kStatsVersion));
  const auto put_u64 = [&out](uint64_t value) {
    for (int i = 0; i < 8; ++i) {
      out.push_back(static_cast<char>((value >> (8 * i)) & 0xFF));
    }
  };
  put_u64(static_cast<uint64_t>(created_at));
  put_u64(static_cast<uint64_t>(last_write_at));
  put_u64(static_cast<uint64_t>(row_count));
  return out;
}

bool decode_stats(const std::string &data, int64_t &created_at,
                  int64_t &last_write_at, int64_t &row_count) {
  if (data.empty()) {
    return false;
  }
  const auto get_u64 = [&data](size_t pos) {
    uint64_t value = 0;
    for (int i = 0; i < 8; ++i) {
      value |= static_cast<uint64_t>(static_cast<unsigned char>(
                   data[pos + static_cast<size_t>(i)]))
               << (8 * i);
    }
    return value;
  };
  const uint8_t version = static_cast<uint8_t>(data[0]);
  if (version == 1 && data.size() == 17) {
    created_at = static_cast<int64_t>(get_u64(1));
    last_write_at = static_cast<int64_t>(get_u64(9));
    row_count = -1; // 旧记录没有行数
    return true;
  }
  if (version == 2 && data.size() == 25) {
    created_at = static_cast<int64_t>(get_u64(1));
    last_write_at = static_cast<int64_t>(get_u64(9));
    row_count = static_cast<int64_t>(get_u64(17));
    return true;
  }
  return false;
}

} // namespace

KVCatalog::KVCatalog(std::shared_ptr<kv::KVEngine> engine, Clock now)
    : engine_(std::move(engine)), now_(now ? std::move(now) : Clock([] {
        return static_cast<int64_t>(
            std::chrono::duration_cast<std::chrono::seconds>(
                std::chrono::system_clock::now().time_since_epoch())
                .count());
      })) {}

// ============================================================
// 会话状态
// ============================================================
bool KVCatalog::is_open() const {
  return engine_ != nullptr && engine_->is_open();
}

Identifier KVCatalog::current_database() const { return current_db_; }

bool KVCatalog::use_database(const Identifier &db_name) {
  if (!database_exists(db_name)) {
    return false;
  }
  current_db_ = db_name;
  return true;
}

// ============================================================
// 元数据读写
// ============================================================
std::string KVCatalog::read_meta(const std::string &key) const {
  if (!is_open()) {
    return {};
  }
  std::string value;
  if (engine_->get(key, &value) != kv::Status::OK) {
    return {};
  }
  return value;
}

bool KVCatalog::write_meta(const std::string &key, const std::string &value) {
  if (!is_open()) {
    return false;
  }
  return engine_->put(key, value) == kv::Status::OK;
}

std::string
KVCatalog::serialize_names(const std::vector<Identifier> &names) const {
  std::string out;
  out.reserve(1 + names.size() * 16);
  out.push_back(static_cast<char>(kNameListVersion));
  for (const auto &name : names) {
    const std::string raw = name.str(); // 保留原始大小写，便于展示
    append_u32_le(out, static_cast<uint32_t>(raw.size()));
    out += raw;
  }
  return out;
}

std::vector<Identifier> KVCatalog::parse_names(const std::string &data) const {
  std::vector<Identifier> names;
  if (data.empty() || static_cast<uint8_t>(data[0]) != kNameListVersion) {
    return names; // 空/版本不符：当作没有元数据
  }
  size_t pos = 1;
  while (pos < data.size()) {
    uint32_t length = 0;
    if (!read_u32_le(data, pos, length)) {
      break;
    }
    if (pos + length > data.size()) {
      break;
    }
    names.emplace_back(data.substr(pos, length));
    pos += length;
  }
  return names;
}

std::vector<Identifier> KVCatalog::list_databases() const {
  return parse_names(read_meta(keys::databases()));
}

std::vector<Identifier>
KVCatalog::list_tables_raw(const Identifier &db_name) const {
  return parse_names(read_meta(keys::db_tables(db_name)));
}

std::vector<Identifier>
KVCatalog::list_tables(const Identifier &db_name) const {
  return list_tables_raw(db_name);
}

bool KVCatalog::write_table_list(const Identifier &db_name,
                                 const std::vector<Identifier> &tables) {
  return write_meta(keys::db_tables(db_name), serialize_names(tables));
}

bool KVCatalog::database_exists(const Identifier &db_name) const {
  const auto dbs = list_databases();
  return std::find(dbs.begin(), dbs.end(), db_name) != dbs.end();
}

bool KVCatalog::table_exists(const Identifier &db_name,
                             const Identifier &table_name) const {
  const auto tables = list_tables_raw(db_name);
  return std::find(tables.begin(), tables.end(), table_name) != tables.end();
}

std::optional<TableSchema>
KVCatalog::get_table_schema(const Identifier &db_name,
                            const Identifier &table_name) const {
  const std::string data = read_meta(keys::db_schema(db_name, table_name));
  if (data.empty()) {
    return std::nullopt;
  }
  auto schema = TableSchema::deserialize(data);
  if (!schema.has_value()) {
    return std::nullopt;
  }
  return std::optional<TableSchema>(std::move(*schema));
}

// ============================================================
// DDL
// ============================================================
bool KVCatalog::create_database(const Identifier &db_name) {
  if (!is_open() || db_name.empty() || database_exists(db_name)) {
    return false;
  }
  auto dbs = list_databases();
  dbs.push_back(db_name);
  // 先写库列表，再写（空的）表列表；表列表写失败就把库列表改回去
  if (!write_meta(keys::databases(), serialize_names(dbs))) {
    return false;
  }
  if (!write_table_list(db_name, {})) {
    dbs.pop_back();
    write_meta(keys::databases(), serialize_names(dbs));
    return false;
  }
  // 建库时间（统计信息）：一个固定长度的记录，不需要名单那套 framing
  DatabaseStats stats;
  stats.created_at = now_();
  write_meta(keys::db_stats(db_name), encode_stats(stats.created_at, 0, -1));
  return true;
}

bool KVCatalog::drop_database(const Identifier &db_name) {
  if (!is_open() || !database_exists(db_name)) {
    return false;
  }
  // 1) 元数据：schema + 表列表
  if (!remove_prefix(keys::db_schema_prefix(db_name))) {
    return false;
  }
  if (!remove_prefix(keys::db_data_prefix(db_name))) {
    return false;
  }
  if (!remove_prefix(keys::table_stats_prefix(db_name))) {
    return false;
  }
  engine_->remove(keys::db_stats(db_name));
  engine_->remove(keys::db_tables(db_name));

  // 2) 从库列表里摘掉
  auto dbs = list_databases();
  const auto it = std::find(dbs.begin(), dbs.end(), db_name);
  if (it != dbs.end()) {
    dbs.erase(it);
  }
  if (!write_meta(keys::databases(), serialize_names(dbs))) {
    return false;
  }
  if (current_db_ == db_name) {
    current_db_ = Identifier();
  }
  return true;
}

bool KVCatalog::create_table(const Identifier &db_name,
                             const TableSchema &schema) {
  if (!is_open() || !database_exists(db_name) || schema.is_empty() ||
      table_exists(db_name, schema.table_name())) {
    return false;
  }
  if (schema.validate() != SchemaError::OK) {
    return false;
  }
  if (!write_meta(keys::db_schema(db_name, schema.table_name()),
                  schema.serialize())) {
    return false;
  }
  write_meta(keys::table_stats(db_name, schema.table_name()),
             encode_stats(now_(), 0, 0)); // 建表时间 + 空表（0 行）
  auto tables = list_tables_raw(db_name);
  tables.push_back(schema.table_name());
  if (!write_table_list(db_name, tables)) {
    engine_->remove(keys::db_schema(db_name, schema.table_name()));
    return false;
  }
  return true;
}

bool KVCatalog::drop_table(const Identifier &db_name,
                           const Identifier &table_name) {
  if (!is_open() || !database_exists(db_name) ||
      !table_exists(db_name, table_name)) {
    return false;
  }
  // 先删数据再删元数据：万一中途失败，至少元数据还在，能重试
  if (!remove_prefix(keys::data_prefix(db_name, table_name))) {
    return false;
  }
  engine_->remove(keys::db_schema(db_name, table_name));
  engine_->remove(keys::table_stats(db_name, table_name));

  auto tables = list_tables_raw(db_name);
  const auto it = std::find(tables.begin(), tables.end(), table_name);
  if (it != tables.end()) {
    tables.erase(it);
  }
  return write_table_list(db_name, tables);
}

// ============================================================
// 统计信息
// ============================================================
std::expected<DatabaseStats, RelError>
KVCatalog::database_stats(const Identifier &db_name) const {
  if (!database_exists(db_name)) {
    return std::unexpected(RelError(RelErrorCode::NOT_FOUND,
                                    "database not found: " + db_name.str()));
  }
  DatabaseStats stats;
  int64_t last_write = 0;
  int64_t rows = -1;
  if (!decode_stats(read_meta(keys::db_stats(db_name)), stats.created_at,
                    last_write, rows)) {
    stats.created_at = 0; // 老数据没有统计记录：不报错，给 0
  }
  return stats;
}

std::expected<TableStats, RelError>
KVCatalog::table_stats(const Identifier &db_name,
                       const Identifier &table_name) const {
  if (!table_exists(db_name, table_name)) {
    return std::unexpected(
        RelError(RelErrorCode::TABLE_NOT_FOUND,
                 "table not found: " + db_name.str() + "." + table_name.str()));
  }
  TableStats stats;
  if (!decode_stats(read_meta(keys::table_stats(db_name, table_name)),
                    stats.created_at, stats.last_write_at, stats.row_count)) {
    stats = TableStats{};
  }
  return stats;
}

bool KVCatalog::touch_table(const Identifier &db_name,
                            const Identifier &table_name, int64_t row_delta) {
  if (!table_exists(db_name, table_name)) {
    return false;
  }
  auto stats = table_stats(db_name, table_name);
  const int64_t created = stats.has_value() ? stats->created_at : 0;
  int64_t rows = stats.has_value() ? stats->row_count : -1;
  if (rows >= 0) {
    rows += row_delta;
    if (rows < 0) {
      rows = 0; // 计数只可能因为漂移为负，兜一下
    }
  }
  return write_meta(keys::table_stats(db_name, table_name),
                    encode_stats(created, now_(), rows));
}

// ============================================================
// 表视图
// ============================================================
std::expected<Table, RelError>
KVCatalog::open_table(const Identifier &db_name,
                      const Identifier &table_name) const {
  if (!is_open()) {
    return std::unexpected(
        RelError(RelErrorCode::NOT_OPEN, "kv engine is not open"));
  }
  auto schema = get_table_schema(db_name, table_name);
  if (!schema.has_value()) {
    return std::unexpected(
        RelError(RelErrorCode::TABLE_NOT_FOUND,
                 "table not found: " + db_name.str() + "." + table_name.str()));
  }
  return Table(engine_, db_name, std::move(*schema));
}

std::expected<Table, RelError>
KVCatalog::open_current_table(const Identifier &table_name) const {
  if (current_db_.empty()) {
    return std::unexpected(
        RelError(RelErrorCode::NOT_OPEN, "no database selected"));
  }
  return open_table(current_db_, table_name);
}

// ============================================================
// 内部：按前缀批量删
// ============================================================
bool KVCatalog::remove_prefix(const std::string &prefix) {
  if (!is_open() || prefix.empty()) {
    return false;
  }
  kv::KeyRange range;
  range.start = prefix;
  range.end = keys::prefix_end(prefix);
  std::vector<std::string> doomed;
  auto it = engine_->new_iterator(range);
  while (it != nullptr && it->valid()) {
    doomed.push_back(it->key());
    it->next();
  }
  if (it != nullptr && it->status() != kv::Status::OK &&
      it->status() != kv::Status::NotFound) {
    return false;
  }
  if (doomed.empty()) {
    return true;
  }
  return engine_->remove_batch(doomed) == kv::Status::OK;
}

} // namespace sql
