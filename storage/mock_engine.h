// mock_engine.h
#ifndef MOCK_ENGINE_H
#define MOCK_ENGINE_H

#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "storage_engine.h"

namespace storage {

class MockEngine : public StorageEngine {
 public:
  MockEngine() = default;
  ~MockEngine() override = default;

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

  // ============================================================
  // 测试辅助
  // ============================================================
  void dump_table(const std::string& table_name);
  void clear();

 private:
  // ============================================================
  // 内部数据结构
  // ============================================================
  struct TableData {
    TableSchema schema;
    std::unordered_map<std::string, std::string> rows;  // key → encoded value
    int64_t next_id;

    TableData() : next_id(1) {}
  };

  std::unordered_map<std::string, TableData> tables_;
  std::mutex mutex_;

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

  // 行操作
  Row build_full_row(const Row& input_row, const TableSchema& schema,
                     const std::vector<std::string>& input_columns);
  bool row_matches_assignments(
      const Row& row, const TableSchema& schema,
      const std::vector<std::pair<std::string, Value>>& assignments);
  Row apply_assignments(
      const Row& row, const TableSchema& schema,
      const std::vector<std::pair<std::string, Value>>& assignments);

  // Key 范围解析
  std::string extract_key_from_full_key(const std::string& full_key,
                                        const std::string& table_name);
  bool key_in_range(const std::string& key, const KeyRange& key_range);

  // 元数据
  TableSchema load_schema(const std::string& table_name);
  void save_schema(const TableSchema& schema);
};

}  // namespace storage

#endif  // MOCK_ENGINE_H