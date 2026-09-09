// query.h
#pragma once

#include <optional>
#include <string>
#include <variant>
#include <vector>

#include "condition.h"
#include "identifier.h"
#include "query_clause.h"
#include "schema.h"
#include "value.h"

namespace sql {

// ============================================================
// 查询类型枚举
// ============================================================
enum class QueryType : uint8_t {
  SELECT,
  INSERT,
  UPDATE,
  DELETE,
  CREATE_TABLE,
  DROP_TABLE,
  CREATE_DATABASE,
  DROP_DATABASE,
  USE_DATABASE,
  UNKNOWN
};

inline const char *query_type_to_string(QueryType type) {
  switch (type) {
  case QueryType::SELECT:
    return "SELECT";
  case QueryType::INSERT:
    return "INSERT";
  case QueryType::UPDATE:
    return "UPDATE";
  case QueryType::DELETE:
    return "DELETE";
  case QueryType::CREATE_TABLE:
    return "CREATE_TABLE";
  case QueryType::DROP_TABLE:
    return "DROP_TABLE";
  case QueryType::CREATE_DATABASE:
    return "CREATE_DATABASE";
  case QueryType::DROP_DATABASE:
    return "DROP_DATABASE";
  case QueryType::USE_DATABASE:
    return "USE_DATABASE";
  default:
    return "UNKNOWN";
  }
}

// ============================================================
// 列引用（用于 SELECT 列表、ORDER BY 等）
// 支持：* （通配符）、表名.列名、列名
// ============================================================
struct ColumnRef {
  // 列名（为空表示 *）
  Identifier column;

  // 表名限定（可选）：table.column
  // 为空表示没有表名前缀
  Identifier table;

  // 别名（可选）：col AS alias
  std::optional<Identifier> alias;

  bool is_wildcard() const { return column.empty() || column == "*"; }
  bool is_qualified() const { return !table.empty(); }

  // 显示名称：优先别名，其次列名
  std::string display_name() const {
    if (alias) {
      return alias->display_name();
    }
    return column.display_name();
  }
};

// ============================================================
// SELECT 语句
// ============================================================
struct SelectQuery {
  // 表引用
  Identifier table;
  std::optional<Identifier> alias; // 表别名（可选）

  // SELECT 列表
  // 空 = SELECT *（或者显式放入通配符）
  std::vector<ColumnRef> columns;
  bool select_all() const {
    return columns.empty() || columns[0].is_wildcard();
  }

  // WHERE 条件（nullptr = 无条件）
  ConditionPtr where;

  // GROUP BY（尚未使用，预留）
  std::vector<Identifier> group_by;

  // ORDER BY
  std::vector<OrderByItem> order_by;

  // LIMIT / OFFSET
  LimitClause limit;

  // 调试
  std::string to_string() const {
    std::string s = "SELECT ";
    if (select_all()) {
      s += "*";
    } else {
      for (size_t i = 0; i < columns.size(); ++i) {
        if (i > 0)
          s += ", ";
        if (columns[i].is_qualified()) {
          s += columns[i].table.str() + ".";
        }
        s += columns[i].column.display_name();
        if (columns[i].alias) {
          s += " AS " + columns[i].alias->display_name();
        }
      }
    }
    s += " FROM " + table.display_name();
    if (alias)
      s += " AS " + alias->display_name();
    if (where)
      s += " WHERE " + where->to_string();
    if (!group_by.empty()) {
      s += " GROUP BY ";
      for (size_t i = 0; i < group_by.size(); ++i) {
        if (i > 0)
          s += ", ";
        s += group_by[i].display_name();
      }
    }
    if (!order_by.empty()) {
      s += " ORDER BY ";
      for (size_t i = 0; i < order_by.size(); ++i) {
        if (i > 0)
          s += ", ";
        s += order_by[i].column.display_name();
        s += (order_by[i].direction == OrderDirection::ASC) ? " ASC" : " DESC";
      }
    }
    if (limit.has_limit()) {
      s += " LIMIT " + std::to_string(limit.limit_value());
    }
    if (limit.has_offset()) {
      s += " OFFSET " + std::to_string(limit.offset_value());
    }
    return s;
  }
};

// ============================================================
// INSERT 语句
// ============================================================
struct InsertQuery {
  Identifier table;

  // 列列表（空 = 所有列）
  std::vector<Identifier> columns;
  bool use_default_columns() const { return columns.empty(); }

  // 单行或多行值
  // values[i][j] = 第 i 行第 j 列的值
  std::vector<std::vector<Value>> values;

  size_t row_count() const { return values.size(); }
  size_t column_count() const {
    if (values.empty()) {
      return 0;
    }
    return values[0].size();
  }

  // 调试
  std::string to_string() const {
    std::string s = "INSERT INTO " + table.display_name();
    if (!columns.empty()) {
      s += " (";
      for (size_t i = 0; i < columns.size(); ++i) {
        if (i > 0)
          s += ", ";
        s += columns[i].display_name();
      }
      s += ")";
    }
    s += " VALUES ";
    for (size_t row = 0; row < values.size(); ++row) {
      if (row > 0)
        s += ", ";
      s += "(";
      for (size_t col = 0; col < values[row].size(); ++col) {
        if (col > 0)
          s += ", ";
        s += values[row][col].to_string();
      }
      s += ")";
    }
    return s;
  }
};

// ============================================================
// UPDATE 语句
// ============================================================

struct UpdateQuery {
  Identifier table;

  // SET 子句：列名 → 新值
  struct Assignment {
    Identifier column;
    Value value;
  };
  std::vector<Assignment> assignments;

  // WHERE 条件（nullptr = 更新所有行）
  ConditionPtr where;

  // 调试
  std::string to_string() const {
    std::string s = "UPDATE " + table.display_name() + " SET ";
    for (size_t i = 0; i < assignments.size(); ++i) {
      if (i > 0)
        s += ", ";
      s += assignments[i].column.display_name() + " = " +
           assignments[i].value.to_string();
    }
    if (where) {
      s += " WHERE " + where->to_string();
    }
    return s;
  }
};

// ============================================================
// DELETE 语句
// ============================================================
struct DeleteQuery {
  Identifier table;

  // WHERE 条件（nullptr = 删除所有行）
  ConditionPtr where;

  // 调试
  std::string to_string() const {
    std::string s = "DELETE FROM " + table.display_name();
    if (where) {
      s += " WHERE " + where->to_string();
    }
    return s;
  }
};

// ============================================================
// DDL 语句
// ============================================================

// CREATE TABLE
struct CreateTableQuery {
  Identifier database; // 数据库名（可空）
  Identifier table;
  std::vector<ColumnDef> columns;

  // 调试
  std::string to_string() const {
    std::string s = "CREATE TABLE ";
    if (!database.empty())
      s += database.display_name() + ".";
    s += table.display_name() + " (";
    for (size_t i = 0; i < columns.size(); ++i) {
      if (i > 0)
        s += ", ";
      s += columns[i].to_string();
    }
    s += ")";
    return s;
  }
};

// DROP TABLE
struct DropTableQuery {
  Identifier database; // 可空
  Identifier table;

  std::string to_string() const {
    std::string s = "DROP TABLE ";
    if (!database.empty())
      s += database.display_name() + ".";
    s += table.display_name();
    return s;
  }
};

// CREATE DATABASE
struct CreateDatabaseQuery {
  Identifier database;

  std::string to_string() const { return "CREATE DATABASE " + database.display_name(); }
};

// DROP DATABASE
struct DropDatabaseQuery {
  Identifier database;

  std::string to_string() const { return "DROP DATABASE " + database.display_name(); }
};

// USE DATABASE
struct UseDatabaseQuery {
  Identifier database;

  std::string to_string() const { return "USE " + database.display_name(); }
};

// ============================================================
// QueryStmt 变体
// ============================================================
using QueryStmt =
    std::variant<SelectQuery, InsertQuery, UpdateQuery, DeleteQuery,
                 CreateTableQuery, DropTableQuery, CreateDatabaseQuery,
                 DropDatabaseQuery, UseDatabaseQuery>;

// ============================================================
// Query 包装类（统一接口）
// ============================================================
class Query {
public:
  // ---- 构造函数（从各语句类型构造） ----
  Query() = delete;

  // 移动构造（编译器自动生成，但可以显式声明）
  Query(Query&& other) noexcept = default;
  Query& operator=(Query&& other) noexcept = default;
  
  // 删除拷贝（明确表达意图）
  // 因为 std::unique_ptr 不可拷贝，所以包含 ConditionPtr的 struct 也不可拷贝。
  // Select, Update, Delete
  Query(const Query&) = delete;
  Query& operator=(const Query&) = delete;

  template <typename T>
  explicit Query(T stmt) : type_(stmt_type<T>()), stmt_(std::move(stmt)) {}

  // ---- 类型判断 ----
  QueryType type() const { return type_; }

  bool is_select() const { return type_ == QueryType::SELECT; }
  bool is_insert() const { return type_ == QueryType::INSERT; }
  bool is_update() const { return type_ == QueryType::UPDATE; }
  bool is_delete() const { return type_ == QueryType::DELETE; }
  bool is_create_table() const { return type_ == QueryType::CREATE_TABLE; }
  bool is_drop_table() const { return type_ == QueryType::DROP_TABLE; }
  bool is_use_database() const { return type_ == QueryType::USE_DATABASE; }
  bool is_create_database() const {
    return type_ == QueryType::CREATE_DATABASE;
  }
  bool is_drop_database() const { return type_ == QueryType::DROP_DATABASE; }

  bool is_ddl() const {
    return type_ == QueryType::CREATE_TABLE || type_ == QueryType::DROP_TABLE ||
           type_ == QueryType::CREATE_DATABASE ||
           type_ == QueryType::DROP_DATABASE;
  }
  bool is_dml() const {
    return type_ == QueryType::SELECT || type_ == QueryType::INSERT ||
           type_ == QueryType::UPDATE || type_ == QueryType::DELETE;
  }
  bool is_ctrl() const { return type_ == QueryType::USE_DATABASE; }

  // ---- 访问器：获取特定类型（类似 get_if，但通过 Query 对象） ----
  const SelectQuery *select() const { return std::get_if<SelectQuery>(&stmt_); }
  SelectQuery *select() { return std::get_if<SelectQuery>(&stmt_); }

  const InsertQuery *insert() const { return std::get_if<InsertQuery>(&stmt_); }
  InsertQuery *insert() { return std::get_if<InsertQuery>(&stmt_); }

  const UpdateQuery *update() const { return std::get_if<UpdateQuery>(&stmt_); }
  UpdateQuery *update() { return std::get_if<UpdateQuery>(&stmt_); }

  const DeleteQuery *delete_() const {
    return std::get_if<DeleteQuery>(&stmt_);
  }
  DeleteQuery *delete_() { return std::get_if<DeleteQuery>(&stmt_); }

  // DDL 访问器
  const CreateTableQuery *create_table() const {
    return std::get_if<CreateTableQuery>(&stmt_);
  }
  CreateTableQuery *create_table() {
    return std::get_if<CreateTableQuery>(&stmt_);
  }

  const DropTableQuery *drop_table() const {
    return std::get_if<DropTableQuery>(&stmt_);
  }
  DropTableQuery *drop_table() { return std::get_if<DropTableQuery>(&stmt_); }

  const CreateDatabaseQuery *create_database() const {
    return std::get_if<CreateDatabaseQuery>(&stmt_);
  }
  CreateDatabaseQuery *create_database() {
    return std::get_if<CreateDatabaseQuery>(&stmt_);
  }

  const DropDatabaseQuery *drop_database() const {
    return std::get_if<DropDatabaseQuery>(&stmt_);
  }
  DropDatabaseQuery *drop_database() {
    return std::get_if<DropDatabaseQuery>(&stmt_);
  }

  const UseDatabaseQuery *use_database() const {
    return std::get_if<UseDatabaseQuery>(&stmt_);
  }
  UseDatabaseQuery *use_database() {
    return std::get_if<UseDatabaseQuery>(&stmt_);
  }

  // 获取目标表名（非 DML 返回 nullptr）
  const Identifier *target_table() const {
    if (auto *s = select())
      return &s->table;
    if (auto *ins = insert())
      return &ins->table;
    if (auto *upd = update())
      return &upd->table;
    if (auto *del = delete_())
      return &del->table;
    if (auto *ct = create_table())
      return &ct->table;
    if (auto *dt = drop_table())
      return &dt->table;
    return nullptr;
  }

  // ---- 内部 query 类型推导 ----
  template <typename T> static QueryType stmt_type() {
    if constexpr (std::is_same_v<T, SelectQuery>)
      return QueryType::SELECT;
    else if constexpr (std::is_same_v<T, InsertQuery>)
      return QueryType::INSERT;
    else if constexpr (std::is_same_v<T, UpdateQuery>)
      return QueryType::UPDATE;
    else if constexpr (std::is_same_v<T, DeleteQuery>)
      return QueryType::DELETE;
    else if constexpr (std::is_same_v<T, CreateTableQuery>)
      return QueryType::CREATE_TABLE;
    else if constexpr (std::is_same_v<T, DropTableQuery>)
      return QueryType::DROP_TABLE;
    else if constexpr (std::is_same_v<T, CreateDatabaseQuery>)
      return QueryType::CREATE_DATABASE;
    else if constexpr (std::is_same_v<T, DropDatabaseQuery>)
      return QueryType::DROP_DATABASE;
    else if constexpr (std::is_same_v<T, UseDatabaseQuery>)
      return QueryType::USE_DATABASE;
    else {
      static_assert(sizeof(T) == 0, "Unsupported query type");
    }
  }

  // query.visit([](const auto& stmt) {
  //   using T = std::decay_t<decltype(stmt)>;
  //   if constexpr (std::is_same_v<T, sql::SelectQuery>) {
  //   std::cout << "SELECT " << stmt.table.str();
  //  }
  // });
  template <typename Visitor> decltype(auto) visit(Visitor &&visitor) const {
    return std::visit(std::forward<Visitor>(visitor), stmt_);
  }

  template <typename Visitor> decltype(auto) visit(Visitor &&visitor) {
    return std::visit(std::forward<Visitor>(visitor), stmt_);
  }

  // ---- 调试 ----
  std::string to_string() const {
  auto current_type = type_; 
  return std::visit(
      [current_type](const auto& stmt) {
          if constexpr (requires { stmt.to_string(); }) {
              return stmt.to_string();
          } else {
              return query_type_to_string(current_type);
          }
      }, 
      stmt_
  );
}

private:
  QueryType type_ = QueryType::UNKNOWN;
  QueryStmt stmt_; // 具体语句内容
};

// ============================================================
// 便捷构造函数（工厂函数）
// ============================================================
//用可变参数模板做完美转发
template <typename T>
inline Query make_query(T&& q) {
    return Query(std::forward<T>(q));
}

} // namespace sql