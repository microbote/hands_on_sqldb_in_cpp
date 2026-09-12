// table.h
//
// Table：一张表的**轻量视图**（不是有状态的 Table 对象）。
//
//   Table{engine, db, schema} 只保存"怎么定位这张表"，所有操作直接落到 KV：
//     行 key   = @data/<db>/<table>/ + KeyCodecs::to_key(pk, 主键列类型)
//     行 value = Row::serialize(schema)（整行一个 blob）
//
//   它不做缓存、不做连接池、不打印日志：错误用 std::expected 返回，
//   由调用方（执行器 / CLI）决定怎么呈现。这样表对象可以随手创建销毁，
//   也便于并发使用。
//
//   扫描返回 Cursor（见 cursor.h），游标不持有 Table 所有权，
//   调用方要保证 Table 比游标活得久。
#pragma once

#include <expected>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "relation_defs.h"
#include "sql_types/cursor.h"
#include "sql_types/identifier.h"
#include "sql_types/key_range.h"
#include "sql_types/row.h"
#include "sql_types/schema.h"
#include "sql_types/value.h"

namespace kv {
class KVEngine;
}

namespace sql {

class TableCursor;  // relation/cursor.h（避免和 sql::Cursor 混淆）

class Table {
public:
  Table(std::shared_ptr<kv::KVEngine> engine, Identifier db,
        TableSchema schema);
  ~Table() = default;

  // ----- 属性 -----
  const TableSchema &schema() const { return schema_; }
  const Identifier &db_name() const { return db_name_; }
  const Identifier &table_name() const { return schema_.table_name(); }
  const std::string &key_prefix() const { return key_prefix_; }

  // 主键列（无主键的表返回 nullptr）
  const ColumnDef *primary_key_column() const;
  DataType primary_key_type() const;

  // ----- 读 -----

  // 点查：不存在返回 nullopt（不算错误）
  std::expected<std::optional<Row>, RelError>
  find(const Value &primary_key) const;

  // 点查：不存在返回 NOT_FOUND 错误
  std::expected<Row, RelError> get(const Value &primary_key) const;

  // 按逻辑主键区间扫描；ascending=false 时反向扫（ORDER BY pk DESC）
  // 返回具体类型：reset()/range() 是存储游标特有的能力，需要时可直接用；
  // 当作 Cursor 用时 unique_ptr 会自动转换。
  std::unique_ptr<TableCursor> scan(const KeyRange &range,
                                    bool ascending = true) const;

  // 全表扫描
  std::unique_ptr<TableCursor> scan_all(bool ascending = true) const;

  // 行数（全表扫一遍，仅供测试/统计）
  std::expected<size_t, RelError> row_count() const;

  // ----- 写 -----

  std::expected<void, RelError> insert(const Row &row);

  // 整行覆写（要求 row 的主键与 primary_key 一致）
  std::expected<void, RelError> update(const Value &primary_key,
                                       const Row &row);

  // 部分列更新：先读出行，改完再写回（不存在返回 NOT_FOUND）
  std::expected<void, RelError>
  update(const Value &primary_key,
         const std::vector<std::pair<Identifier, Value>> &assignments);

  std::expected<void, RelError> remove(const Value &primary_key);

  // 清空本表所有数据 key
  std::expected<void, RelError> truncate();

  // ----- 编解码（执行器/测试可直接用）-----

  // 主键 -> 数据 key（带表前缀，保序）
  std::string encode_key(const Value &primary_key) const;

  std::string encode_row(const Row &row) const;

  std::expected<Row, RelError> decode_row(std::string_view data) const;

  // 行里的主键值（主键列缺失时返回 NULL）
  Value primary_key_of(const Row &row) const;

private:
  // 逻辑范围 -> 物理 KV 范围（加表前缀）
  std::string physical_start(const KeyRange &range) const;
  std::string physical_end(const KeyRange &range) const;

  std::shared_ptr<kv::KVEngine> engine_;
  Identifier db_name_;
  TableSchema schema_;
  std::string key_prefix_;
  DataType primary_key_type_ = DataType::UNKNOWN_TYPE;
};

} // namespace sql
