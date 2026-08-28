// table.h
#pragma once

#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "key_prefix.h"
#include "row.h"
#include "schema.h"
#include "storage/kv_engine/kv_engine.h"

namespace sql {

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

  // ===== CRUD 操作 =====

  // 插入一行
  bool insert(const Row& row);
  bool insert(const std::vector<Value>& values);

  // 批量插入
  bool insert_batch(const std::vector<Row>& rows);

  // 根据主键查询
  std::optional<Row> get(const Value& primary_key);
  std::optional<Row> get(const std::string& primary_key);
  std::optional<Row> get(int64_t primary_key);

  // 根据主键更新
  bool update(const Value& primary_key, const Row& row);
  bool update(const Value& primary_key,
              const std::vector<std::pair<std::string, Value>>& assignments);

  // 根据主键删除
  bool remove(const Value& primary_key);
  bool remove(const std::string& primary_key);
  bool remove(int64_t primary_key);

  // ===== 扫描 =====

  // 全表扫描
  std::vector<Row> scan_all();

  // 范围扫描（按主键）
  std::vector<Row> scan_range(const Value& start, const Value& end);
  std::vector<Row> scan_prefix(const std::string& prefix);

  // ===== 统计 =====
  size_t row_count() const;

  // ===== 清空 =====
  bool truncate();

 private:
  // ----- 辅助方法 -----
  Value get_primary_key(const Row& row) const;
  std::string encode_row(const Row& row) const;
  Row decode_row(const std::string& data) const;

  std::shared_ptr<kv::KVEngine> engine_;
  std::string db_name_;
  TableSchema schema_;
  std::string key_prefix_;
};

}  // namespace sql