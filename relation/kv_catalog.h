// kv_catalog.h
//
// KVCatalog：sql::Catalog 在 KV 存储上的实现（relation 层的主入口）。
//
//   - 元数据（库列表 / 表列表 / schema）都存在 KV 里，见 key_prefix.h：
//       @system/databases                 -> 库名列表
//       @system/tables/<db>               -> 表名列表
//       @system/schema/<db>/<table>       -> TableSchema::serialize()
//   - 会话状态只有"当前数据库"（USE 语义），放在内存里，不落 KV；
//   - 表本身没有状态：open_table() 返回一个轻量的 Table 视图（见 table.h），
//     执行器拿它去 scan/get/insert；
//   - 没有 Database/TableManager 这类"活对象"：它们只是元数据的视图，
//     缓存反而会带来一致性问题。
//
// 错误约定：Catalog 接口本身返回 bool/optional（接口是 sql_types 定的），
// relation 自己的操作（open_table）用 std::expected<_, RelError>。
#pragma once

#include <cstdint>
#include <expected>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "relation_defs.h"
#include "sql_types/catalog.h"
#include "sql_types/identifier.h"
#include "sql_types/schema.h"
#include "table.h"

namespace kv {
class KVEngine;
}

namespace sql {

// ============================================================
// 统计信息（元命令等客户端展示用）
// ============================================================
struct DatabaseStats {
  int64_t created_at = 0; // 建库时间（Unix 秒）
};

struct TableStats {
  int64_t created_at = 0;    // 建表时间（Unix 秒）
  int64_t last_write_at = 0; // 最近一次改了这张表的写语句时间（0 = 还没写过）
  // 维护着的行数：只给优化器估算用（-1 = 未知）。
  // 元命令 \dt / \d 显示的行数仍然是现算的，保证看到的是真值。
  int64_t row_count = -1;
};

class KVCatalog : public Catalog {
public:
  // 时间源可注入：默认取系统时间；测试里给个假时钟就能做确定性断言
  using Clock = std::function<int64_t()>;

  explicit KVCatalog(std::shared_ptr<kv::KVEngine> engine, Clock now = {});
  ~KVCatalog() override = default;

  // ===== sql::Catalog 接口 =====
  bool is_open() const override;
  Identifier current_database() const override;

  bool database_exists(const Identifier &db_name) const override;
  std::vector<Identifier> list_databases() const override;

  bool table_exists(const Identifier &db_name,
                    const Identifier &table_name) const override;
  std::vector<Identifier> list_tables(const Identifier &db_name) const override;
  std::optional<TableSchema>
  get_table_schema(const Identifier &db_name,
                   const Identifier &table_name) const override;

  bool create_database(const Identifier &db_name) override;
  bool drop_database(const Identifier &db_name) override;
  bool create_table(const Identifier &db_name,
                    const TableSchema &schema) override;
  bool drop_table(const Identifier &db_name,
                  const Identifier &table_name) override;

  // ===== relation 扩展 =====

  // USE：只改本对象的会话状态（不落 KV）
  bool use_database(const Identifier &db_name);

  // 拿一张表的视图（执行器的入口）
  std::expected<Table, RelError> open_table(const Identifier &db_name,
                                            const Identifier &table_name) const;

  // 用当前数据库打开表
  std::expected<Table, RelError>
  open_current_table(const Identifier &table_name) const;

  // ---- 统计信息 ----
  std::expected<DatabaseStats, RelError>
  database_stats(const Identifier &db_name) const;
  std::expected<TableStats, RelError>
  table_stats(const Identifier &db_name, const Identifier &table_name) const;

  // 写语句改了表之后由上层调用（session 用）：更新"最后写入时间"，
  // 并按 delta 调整行数（INSERT +n / DELETE -n / UPDATE 传 0）
  bool touch_table(const Identifier &db_name, const Identifier &table_name,
                   int64_t row_delta = 0);

  kv::KVEngine *engine() const { return engine_.get(); }

private:
  // 读元数据值；key 不存在返回空串
  std::string read_meta(const std::string &key) const;
  bool write_meta(const std::string &key, const std::string &value);

  // 名单的序列化/反序列化（长度前缀 framing，见 .cpp）
  std::string serialize_names(const std::vector<Identifier> &names) const;
  std::vector<Identifier> parse_names(const std::string &data) const;

  std::vector<Identifier> list_tables_raw(const Identifier &db_name) const;
  bool write_table_list(const Identifier &db_name,
                        const std::vector<Identifier> &tables);

  // 删掉某个前缀下的所有 key（DROP TABLE / DROP DATABASE 用）
  bool remove_prefix(const std::string &prefix);

  std::shared_ptr<kv::KVEngine> engine_;
  Identifier current_db_; // 会话状态：当前数据库（USE）
  Clock now_;             // 时间源（默认系统时间）
};

} // namespace sql
