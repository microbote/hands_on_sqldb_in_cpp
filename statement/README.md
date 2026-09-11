# statement 模块

把 parser 产出的 AST 变成 **sql_types 的 `sql::Query`**，并在执行前做语义校验。

```
SQL 文本 --parser--> AST --StatementBuilder--> sql::Query --StatementValidator--> OK / StmtError
                                                              (查 sql::Catalog)
```

## 两个类的职责

| 类 | 输入 | 输出 | 做什么 | 不做什么 |
|----|------|------|--------|----------|
| `StatementBuilder` | `ASTNode*` | `std::expected<sql::Query, StmtError>` | 节点结构转换、结构校验（字段是否齐全、值节点是否合法）、SQL 语义规范化（如 PRIMARY KEY ⇒ NOT NULL） | 不查 Catalog、不碰存储、**不接管 AST 所有权** |
| `StatementValidator` | `const sql::Query&` + `const sql::Catalog&` | `std::expected<void, StmtError>` | 库/表/列是否存在、值类型族与取值范围、主键约束、DDL 语义（重复建库/建表、删不存在的对象） | 不修改数据；**校验通过 ≠ 已执行** |

调用方（未来的执行器/shell）需要自己把通过校验的 DDL 落到 Catalog 上，
或者走上层事务接口 —— validator 是只读的。

两个类都**不保存错误状态**：错误码与错误信息随返回值一起走，
因此同一个 builder/validator 可以反复调用、并发使用，
也不会出现"上一次的错误粘住下一次调用"（早期用成员 `error_` 的版本就有这个 bug）。

## 错误处理（`stmt_defs.h`）

```cpp
enum class StmtErrorCode : uint8_t {
  OK = 0,
  // 校验类 1..49
  UNKNOWN_STMT_TYPE, VALIDATOR_NOT_INIT, DATABASE_NOT_FOUND,
  DATABASE_ALREADY_EXISTS, TABLE_NOT_FOUND, TABLE_ALREADY_EXISTS,
  COLUMN_NOT_FOUND, COLUMN_COUNT_MISMATCH, COLUMN_TYPE_MISMATCH,
  VALUE_OUT_OF_RANGE, COLUMN_ATTR_NULL_MISMATCH, DUPLICATE_COLUMN,
  DUPLICATE_PRIMARY_KEY, NO_PRIMARY_KEY, INVALID_SCHEMA, EMPTY_STATEMENT,
  // 构建类 99..
  AST_IS_NULL = 99, UNKNOWN_AST_Type, INVALID_AST_NODE, UNSUPPORTED_AST_NODE,
};

struct StmtError {              // 错误是"值"：码 + 上下文信息
  StmtErrorCode code = StmtErrorCode::OK;
  std::string message;
  bool ok() const;
  explicit operator bool() const;
  std::string to_string() const;   // message 为空时退回 stmt_error_message(code)
};
```

用法：

```cpp
stmt::StatementBuilder builder;
auto query = builder.build(ast);                 // expected<Query, StmtError>
if (!query) {
  log(query.error().code, query.error().message);
  return;
}
stmt::StatementValidator validator(catalog);
if (auto ok = validator.validate(*query); !ok) { // expected<void, StmtError>
  log(ok.error().to_string());
}
```

为什么不用 `struct { bool ok; StmtErrorCode error; std::string message;
std::optional<Query> query; }` 这种"结果结构体"：`ok`/`error`/`query.has_value()`
三者可以互相矛盾且无法约束；它本质是手写的 `expected`，会丢掉
`and_then/or_else/value_or` 等设施，并让模块内出现两种风格。
把"信息"放进错误类型本身，既保住了 `expected` 的现代用法，
又让错误码（控制流）与消息（日志/CLI）各归其位。

## AST → Query 映射

| AST 节点 | 产物 |
|----------|------|
| `NODE_USE` | `sql::UseDatabaseQuery` |
| `NODE_SELECT` | `sql::SelectQuery`（列列表、WHERE、ORDER BY、LIMIT） |
| `NODE_INSERT` | `sql::InsertQuery`（列清单可空、VALUES 一行） |
| `NODE_UPDATE` | `sql::UpdateQuery`（SET 列表、WHERE） |
| `NODE_DELETE` | `sql::DeleteQuery`（WHERE） |
| `NODE_CREATE_TABLE` | `sql::CreateTableQuery`（`ColumnDef` 含声明长度） |
| `NODE_DROP_TABLE` | `sql::DropTableQuery` |
| `NODE_CREATE_DATABASE` / `NODE_DROP_DATABASE` | `CreateDatabaseQuery` / `DropDatabaseQuery` |

值节点：`NODE_NUMBER`→整数、`NODE_STRING`→VARCHAR、`NODE_LITERAL`→NULL/TRUE/FALSE
（**NULL 与空串是两回事**）。

比较操作符通过 `sql::from_c()` 从 `COpType` 转换（桥接函数在
`sql_types/compare_op.h`，不要在 statement 层再写字符串映射）。

## 与 Catalog 的关系

`sql::Catalog` 除元数据外还提供会话状态：

```cpp
virtual bool is_open() const;
virtual sql::Identifier current_database() const;
```

不带库名的表（SELECT/INSERT/UPDATE/DELETE）用 `current_database()` 解析；
`StatementValidator` 构造时若 `!catalog.is_open()`，任何 `validate()` 都返回
`VALIDATOR_NOT_INIT`。

## 校验规则一览

| 语句 | 规则 |
|------|------|
| `USE` / `DROP DATABASE` | 库必须存在 |
| `CREATE DATABASE` | 库不能已存在 |
| `CREATE TABLE` | 库存在、表不存在、schema 合法（有主键、列名不重复、声明长度合法） |
| `DROP TABLE` | 库与表都存在 |
| `SELECT` | 表存在；SELECT 列表 / WHERE / ORDER BY 引用的列都存在 |
| `INSERT` | 表存在；显式列清单里的列存在；值个数与列数一致；值类型族与范围匹配（含 `VARCHAR(n)` 长度、TINYINT 范围）；NOT NULL 列必须有值 |
| `UPDATE` | 表存在；SET 的列存在且类型匹配；主键列不允许更新；WHERE 引用的列存在 |
| `DELETE` | 表存在；WHERE 引用的列存在 |

列值校验直接复用 `sql::TableSchema::validate_value()`，
所以类型族、整数宽度、字符串长度、时间类型、NULL 约束的规则与 sql_types 完全一致，
不会出现"两套规则不一致"。

## 构建与测试

```bash
cmake --build build -j4
ctest --test-dir build -R test_statement --output-on-failure
make statement-test        # 等价
```

测试在 `tests/test_statement/`：

- `test_builder.cpp`：AST → Query 的字段级断言（含 NULL/TRUE/FALSE/负数、`VARCHAR(32)`）。
- `test_validator.cpp`：每条校验规则的命中与放行（含只读语义：校验不执行 DDL）。
- `test_pipeline.cpp`：SQL 文本 → 解析 → 构建 → 校验 → 执行 的端到端流程。
- `memory_catalog.h`：测试用的内存 Catalog 实现。
