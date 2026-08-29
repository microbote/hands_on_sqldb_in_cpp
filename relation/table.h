// table.h
#pragma once

#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "primary_key_range.h"
#include "key_prefix.h"
#include "row.h"
#include "schema.h"
#include "storage/kv_engine/kv_engine.h"

namespace sql {

class Cursor;

enum class TableMissingKeyPolicy {
  kReturnEmpty,
  kReturnError,
};

// ============================================================
// 表抽象
// ============================================================
class Table {
 public:
  Table(std::shared_ptr<kv::KVEngine> engine, const std::string& db_name,
        const TableSchema& schema);
  ~Table() = default;

  // ----- 属性 -----
  const TableSchema& schema() const { return schema_; }
  const std::string& name() const { return schema_.name(); }
  const std::string& db_name() const { return db_name_; }

  // ===== 查询接口 =====

  // 主键点查询
  std::optional<Row> get(const Value& primary_key) const;
  std::optional<Row> get(const std::string& primary_key) const;
  std::optional<Row> get(int64_t primary_key) const;

  // 批量点查询
  bool get_batch(const std::vector<Value>& primary_keys,
                             TableMissingKeyPolicy policy,
                             std::vector<std::optional<Row>>* values
                            ) const;

  bool exists(const Value& primary_key) const{
    return get(primary_key).has_value();
  }
  bool exists(const std::string& primary_key) const{
    return get(primary_key).has_value();
  }

  // 范围扫描（返回游标）
  std::unique_ptr<Cursor> scan(const PrimaryKeyRange& range);

  // 全表扫描
  std::unique_ptr<Cursor> scan_all() ;



  // ===== 修改接口 =====

  // 插入
  bool insert(const Row& row);
  bool insert(const std::vector<Value>& values);
  bool insert_batch(const std::vector<Row>& rows);

  // 根据主键更新
  bool update(const Value& primary_key, const Row& row);

  // 更新（部分列）
  bool update(const Value& primary_key,
              const std::vector<std::pair<std::string, Value>>& assignments);

  // 删除
  bool remove(const Value& primary_key);
  bool remove(const std::string& primary_key);
  bool remove(int64_t primary_key);

  // ===== 统计 =====
  size_t row_count() const;

  // ===== 清空 =====
  bool truncate();

  // ----- 辅助方法 -----
  std::string encode_row(const Row& row) const;
  Row decode_row(const std::string& data) const;
  Value get_primary_key(const Row& row) const;
  std::string get_key_prefix() const { return key_prefix_; }


  // 将逻辑范围转换为 KV 物理范围
  inline std::string to_kv_key(const std::string& primary_key) const{
    return keys::data_prefix_key(key_prefix_, primary_key);
  }
  inline std::string to_kv_key(const Value& primary_key) const{
    return keys::data_prefix_key(key_prefix_, primary_key);
  }
  kv::KeyRange to_kv_range(const PrimaryKeyRange& range) const;

 private:
  // ----- 成员 -----
  std::shared_ptr<kv::KVEngine> engine_;
  std::string db_name_;
  TableSchema schema_;
  std::string key_prefix_;
};


}  // namespace sql