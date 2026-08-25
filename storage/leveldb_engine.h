// leveldb_engine.h
#ifndef LEVELDB_ENGINE_H
#define LEVELDB_ENGINE_H

#include <leveldb/db.h>
#include <leveldb/iterator.h>
#include <leveldb/write_batch.h>

#include <atomic>
#include <memory>
#include <mutex>
#include <unordered_map>

#include "storage_engine.h"

namespace storage {

class LevelDBEngine : public StorageEngine {
 public:
  explicit LevelDBEngine(const std::string& path);
  ~LevelDBEngine() override;

  // ============================================================
  // KVStore 接口实现
  // ============================================================
  bool put(const std::string& key, const std::string& value) override;
  bool get(const std::string& key, std::string& value) override;
  bool del(const std::string& key) override;
  bool batch_put(const std::vector<std::pair<std::string, std::string>>&
                     kv_pairs) override;

  std::vector<std::pair<std::string, std::string>> scan(
      const std::string& start, const std::string& end) override;
  std::vector<std::pair<std::string, std::string>> scan_prefix(
      const std::string& prefix) override;

  void flush() override;
  void close() override;

  // ============================================================
  // TableStore 接口实现
  // ============================================================
  bool create_table(const TableSchema& schema) override;
  bool drop_table(const std::string& table_name) override;
  bool table_exists(const std::string& table_name) override;
  std::vector<std::string> list_tables() override;
  TableSchema get_table_schema(const std::string& table_name) override;

  bool insert(const std::string& table_name, const Row& row) override;
  bool insert_batch(const std::string& table_name,
                    const std::vector<Row>& rows) override;

  bool get_by_key(const std::string& table_name, const std::string& primary_key,
                  Row& row) override;
  std::vector<Row> get_by_keys(const std::string& table_name,
                               const KeySet& keys) override;
  std::vector<Row> get_by_key_range(const std::string& table_name,
                                    const KeyRange& key_range) override;
  std::vector<Row> scan_all(const std::string& table_name) override;

  bool update_by_key(
      const std::string& table_name, const std::string& primary_key,
      const std::vector<std::pair<std::string, Value>>& assignments) override;
  bool update_by_keys(
      const std::string& table_name, const KeySet& keys,
      const std::vector<std::pair<std::string, Value>>& assignments) override;
  bool update_by_key_range(
      const std::string& table_name, const KeyRange& key_range,
      const std::vector<std::pair<std::string, Value>>& assignments) override;

  bool delete_by_key(const std::string& table_name,
                     const std::string& primary_key) override;
  bool delete_by_keys(const std::string& table_name,
                      const KeySet& keys) override;
  bool delete_by_key_range(const std::string& table_name,
                           const KeyRange& key_range) override;

  int64_t get_row_count(const std::string& table_name) override;

 private:
  // ============================================================
  // 内部辅助方法
  // ============================================================
  // 编码/解码
  std::string encode_key(const std::string& table_name,
                         const std::string& primary_key);
  std::string encode_key_prefix(const std::string& table_name);
  std::string encode_row(const Row& row, const TableSchema& schema);
  Row decode_row(const std::string& data, const TableSchema& schema);

  // 主键操作
  std::string get_primary_key(const Row& row, const TableSchema& schema);
  std::string generate_next_id(const std::string& table_name);
  bool has_primary_key(const Row& row, const TableSchema& schema);

  // 行操作
  Row build_full_row(const Row& input_row, const TableSchema& schema,
                     const std::vector<std::string>& input_columns);
  Row apply_assignments(
      const Row& row, const TableSchema& schema,
      const std::vector<std::pair<std::string, Value>>& assignments);

  // Key 范围解析
  bool key_in_range(const std::string& key, const KeyRange& key_range);
  std::string extract_pk_from_key(const std::string& key,
                                  const std::string& table_name);

  // 元数据操作
  std::string get_tables_meta_key();
  std::string get_schema_meta_key(const std::string& table_name);
  void load_schema_cache();
  void save_table_list(const std::vector<std::string>& tables);
  std::vector<std::string> load_table_list();
  void save_schema(const TableSchema& schema);
  TableSchema load_schema(const std::string& table_name);

  // 批量操作辅助
  bool batch_put_internal(
      leveldb::WriteBatch* batch, const std::string& table_name,
      const std::vector<std::pair<std::string, std::string>>& kv_pairs);
  bool batch_delete_internal(leveldb::WriteBatch* batch,
                             const std::string& table_name,
                             const std::vector<std::string>& keys);

  // 获取迭代器
  leveldb::Iterator* new_iterator() const;

  // ============================================================
  // 成员变量
  // ============================================================
  std::unique_ptr<leveldb::DB> db_;
  std::string db_path_;
  std::unordered_map<std::string, TableSchema> schema_cache_;
  std::mutex mutex_;
  std::atomic<bool> is_open_;

  // 元数据 Key 前缀
  static const std::string META_TABLES_KEY;
  static const std::string META_SCHEMA_PREFIX;
  static const std::string DATA_PREFIX;
};

}  // namespace storage

#endif  // LEVELDB_ENGINE_H