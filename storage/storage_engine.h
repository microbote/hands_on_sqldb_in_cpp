// storage_engine.h - 纯接口
#ifndef STORAGE_ENGINE_H
#define STORAGE_ENGINE_H

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace storage {

// ============ 数据类型 ============
enum class DataType { INTEGER, STRING, BOOLEAN, NULL_TYPE };

// ============ 值类型 ============
struct Value {
  DataType type;
  union {
    int64_t int_val;
    bool bool_val;
  };
  std::string str_val;

  Value() : type(DataType::NULL_TYPE), int_val(0) {}
  Value(int64_t v) : type(DataType::INTEGER), int_val(v) {}
  Value(bool v) : type(DataType::BOOLEAN), bool_val(v) {}
  Value(const std::string& v) : type(DataType::STRING), str_val(v) {}
  Value(const char* v) : type(DataType::STRING), str_val(v) {}

  std::string to_string() const;
  static Value from_string(const std::string& str, DataType type);
};

// ============ 行数据 ============
using Row = std::vector<Value>;
using RowPtr = std::shared_ptr<Row>;

// ============ 表结构 ============
struct ColumnDef {
  std::string name;
  DataType type;
  bool nullable;
  bool primary_key;

  ColumnDef() : nullable(true), primary_key(false) {}
  ColumnDef(const std::string& n, DataType t)
      : name(n), type(t), nullable(true), primary_key(false) {}
};

struct TableSchema {
  std::string table_name;
  std::vector<ColumnDef> columns;
  int primary_key_index;

  TableSchema() : primary_key_index(-1) {}

  int get_column_index(const std::string& name) const;
  const ColumnDef* get_column(const std::string& name) const;
  std::string get_primary_key_column() const;
};

// ============ Key Range ============
struct KeyRange {
  std::string start;  // 起始 key（包含）
  std::string end;    // 结束 key（不包含）
  bool has_start;
  bool has_end;

  KeyRange() : has_start(false), has_end(false) {}
  KeyRange(const std::string& s, const std::string& e)
      : start(s), end(e), has_start(true), has_end(true) {}

  bool is_all() const { return !has_start && !has_end; }
};

// ============ Key Set (用于点查询) ============
using KeySet = std::vector<std::string>;

// ============ 纯 KV 存储接口 ============
class KVStore {
 public:
  virtual ~KVStore() = default;

  // 基本 KV 操作
  virtual bool put(const std::string& key, const std::string& value) = 0;
  virtual bool get(const std::string& key, std::string& value) = 0;
  virtual bool del(const std::string& key) = 0;
  virtual bool batch_put(
      const std::vector<std::pair<std::string, std::string>>& kv_pairs) = 0;

  // 范围扫描
  virtual std::vector<std::pair<std::string, std::string>> scan(
      const std::string& start, const std::string& end) = 0;
  virtual std::vector<std::pair<std::string, std::string>> scan_prefix(
      const std::string& prefix) = 0;

  // 管理
  virtual void flush() = 0;
  virtual void close() = 0;
};

// ============ 表抽象接口 (基于 KV) ============
class TableStore {
 public:
  virtual ~TableStore() = default;

  // ===== 表操作 =====
  virtual bool create_table(const TableSchema& schema) = 0;
  virtual bool drop_table(const std::string& table_name) = 0;
  virtual bool table_exists(const std::string& table_name) = 0;
  virtual std::vector<std::string> list_tables() = 0;
  virtual TableSchema get_table_schema(const std::string& table_name) = 0;

  // ===== 数据操作 =====
  // 插入一行（自动生成主键）
  virtual bool insert(const std::string& table_name, const Row& row) = 0;

  // 批量插入
  virtual bool insert_batch(const std::string& table_name,
                            const std::vector<Row>& rows) = 0;

  // 根据主键获取一行
  virtual bool get_by_key(const std::string& table_name,
                          const std::string& primary_key, Row& row) = 0;

  // 根据主键列表获取多行
  virtual std::vector<Row> get_by_keys(const std::string& table_name,
                                       const KeySet& keys) = 0;

  // 根据主键范围扫描
  virtual std::vector<Row> get_by_key_range(const std::string& table_name,
                                             const KeyRange& key_range) = 0;

  // 获取所有行
  virtual std::vector<Row> scan_all(const std::string& table_name) = 0;

  // 更新：根据主键更新
  virtual bool update_by_key(
      const std::string& table_name, const std::string& primary_key,
      const std::vector<std::pair<std::string, Value>>& assignments) = 0;

  virtual bool update_by_keys(
      const std::string& table_name, const KeySet& keys,
      const std::vector<std::pair<std::string, Value>>& assignments) = 0;

  virtual bool update_by_key_range(
      const std::string& table_name, const KeyRange& key_range,
      const std::vector<std::pair<std::string, Value>>& assignments) = 0;

  // 删除：根据主键删除
  virtual bool delete_by_key(const std::string& table_name,
                             const std::string& primary_key) = 0;

  // 批量删除
  virtual bool delete_by_keys(const std::string& table_name,
                              const KeySet& keys) = 0;

  virtual bool delete_by_key_range(const std::string& table_name,
                                   const KeyRange& key_range) = 0;

  // ===== 统计信息 =====
  virtual int64_t get_row_count(const std::string& table_name) = 0;
};

// ============ 完整存储引擎 ============
class StorageEngine : public KVStore, public TableStore {
 public:
  virtual ~StorageEngine() = default;
};

// ============ 存储引擎工厂 ============
enum class StorageType {
  MOCK,
  LEVELDB
};

struct StorageOptions{
    std::string path;
    StorageType type;
};

class StorageEngineFactory {
 public:
  static std::unique_ptr<StorageEngine> create_engine(
      const StorageOptions& options);
};

}  // namespace storage

#endif  // STORAGE_ENGINE_H