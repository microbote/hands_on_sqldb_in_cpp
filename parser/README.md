# parser 模块

C 风格的词法（Flex）＋语法（Bison）分析器，产出 AST。

## 职责与边界

```
SQL 文本 --sql.l--> token --sql.y--> AST（C 结构体）
                                       |
                                       +--> statement 模块（C++）: AST -> sql::ColumnDef / Value / Query
                                                |
                                                +--> sql_types: 类型语义与取值范围校验
```

- **parser 只负责"解析正确"**：识别类型名、别名与可选长度，产出 AST。
  长度的**语义**校验（是否合法、是否超限）由 `sql_types` / statement 层负责。
- parser **不依赖 C++ 与 sql_types**：只 include C 头 `common/c_types.h`。
  AST 是 `extern "C"` + POD，可以被 C/C++ 两侧复用。
- 例外：长度上限这类"语法级"检查（`CHAR(300)`、`TEXT(10)`）在解析期就报错，
  用的是与 sql_types 同一份常量，不重复实现规则。

## 类型定义的唯一数据源

类型名/别名、长度上限都只在 **`common/c_types.h`** 定义一次：

| 内容 | 位置 | 使用方 |
|------|------|--------|
| 类型名/别名表 | `c_type_aliases[]` | `sql.l`（标识符 → `TOK_TYPE_NAME`）、`sql_types::string_to_data_type` |
| 长度上限 | `CTYPE_MAX_CHAR_LEN` / `CTYPE_MAX_VARCHAR_LEN` / `CTYPE_MAX_TEXT_LEN` | `sql.y`（长度校验）、`sql_types::kMax*Length` |
| 查表/校验函数 | `c_type_lookup()` / `c_type_length_valid()` / `c_type_accepts_length()` | 同上 |

剩下的必要重复只有 `DataType ↔ CDataType` 两张 switch（C/C++ 解耦的代价），
由 `tests/test_parser/test_type_parsing.cpp` 的
`TypeAliasTablesStayInSync` 用例保证两边一致。

新增一个类型时只需要改：`common/c_types.h` 的枚举与别名表，
以及 `sql_types::DataType` 与 `to_c/from_c`（如果它是新的类型族）。

## 构建

```bash
# 需要 bison >= 3.0（系统自带的 /usr/bin/bison 是 2.3，不可用）
cmake -S . -B build -DCMAKE_C_COMPILER=clang-mp-23 -DCMAKE_CXX_COMPILER=clang++-mp-23
cmake --build build -j4
ctest --test-dir build -R 'test_parser|test_sql_types' --output-on-failure
```

- bison 路径在 `CMakeLists.txt` 里默认配置为 `/usr/local/opt/bison/bin/bison`。
- 生成物（`parser.tab.c/h`、`lex.yy.c/h`）输出到 **`build/parser/`**，
  源码目录不再保留生成文件；`parser.cpp` 用 `extern "C" { #include ... }`
  引入生成头，避免"源码目录里的过期生成物"被优先命中。
- 旧的 `Makefile` 仍会把生成物写到 `parser/`（legacy 流程），两者互不干扰；
  如果混用，注意源码目录里的生成物会优先被 `#include "..."` 命中。

## 支持矩阵

### 已支持

| 类别 | 内容 |
|------|------|
| DDL | `CREATE/DROP DATABASE`、`CREATE/DROP TABLE`、内联 `PRIMARY KEY`、`NOT NULL` / `NULL` |
| DML | `SELECT`（含 `WHERE`/`ORDER BY`/`LIMIT`/`OFFSET`）、`INSERT`（多行值/列列表）、`UPDATE`、`DELETE` |
| 条件 | `= != <> > >= < <=`、`AND/OR/NOT`、`IN/NOT IN`、`LIKE/NOT LIKE`、`IS [NOT] NULL`、括号分组 |
| 类型 | `TINYINT/INT8`、`SMALLINT/INT16`、`INT/INTEGER/INT32`、`BIGINT/INT64`、`CHAR(n)`、`VARCHAR(n)`/`VARYING`、`TEXT`、`BOOLEAN/BOOL`、`DATE`、`TIME`、`DATETIME/TIMESTAMP` |
| 字面量 | 整数（int64，越界报错）、负数、`NULL`、`TRUE`/`FALSE`、字符串（支持 `''` 转义） |

### 暂不支持（会报语法错误，不会静默忽略）

| 内容 | 说明 |
|------|------|
| 表级约束 | `PRIMARY KEY (a, b)`、`UNIQUE`、`CHECK`、`REFERENCES` |
| 列默认值 | `DEFAULT ...`、`AUTO_INCREMENT` |
| `IF NOT EXISTS` / `IF EXISTS` | DDL 幂等语法 |
| 双引号/反引号标识符 | `"col"`、`` `col` ``；字符串请用单引号 |
| 字符串反斜杠转义 | `'\n'`、`\'`（标准 SQL 的 `''` 已支持） |
| 其它 | JOIN、子查询、`DISTINCT`、聚合、`GROUP BY`/`HAVING`、`BETWEEN`、算术表达式 |

## 错误处理约定

- **库代码不打印任何东西**：需要日志的调用方用
  `parser::Parser::set_log_callback()` 注册回调（CLI 需要时自行注册）。
- 只记录**第一个**错误：词法层给出的具体错误（例如
  `integer literal out of range`）不会被随后的 `syntax error` 覆盖。
- 行号来自 flex 的 `yylineno`（`%option yylineno`），错误信息形如
  `line 4: syntax error, unexpected ...`。
- 未知字符不再被吞掉：交给语法层报错（例如 `-5` 会解析成负数而不是 `5`，
  `"t"` 会报语法错误而不是被当成 `t`）。

## 实现注意

- `parser.cpp` 里对生成代码的声明必须使用 C 链接（`extern "C"`），
  因为 `parser.tab.c` / `lex.yy.c` 是按 C 编译的。
- `yyerror` 由 `sql.y` 声明、`parser.cpp` 定义（C 链接）。
- `LIMIT/OFFSET` 的 AST 字段是 `int`，解析时做范围检查。
- AST 节点注册表 `nodes[]` 按 `NodeType` 顺序排列，并有编译期长度检查，
  新增节点时同时改 `NodeType`、结构体与注册表。
