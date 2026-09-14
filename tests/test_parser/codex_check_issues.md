# parser 模块审查（针对 sql_types 新增类型的适配）

审查范围：`parser/sql.l`、`parser/sql.y`、`parser/ast.h`、`parser/ast.cpp`、
`parser/parser.h/.cpp`、`statement/statement_builder.h`、`common/c_types.h`。

验证方式（未改动仓库文件，全部在 /tmp 的副本上做）：
用 `/usr/local/opt/bison/bin/bison 3.8.2` + `flex` 重新生成，配合
`clang++-mp-23 -std=c++23` 编译，写了一个最小驱动真跑语句；
另用 `/usr/bin/bison`（2.3）交叉验证旧行为。

结论：**parser 目前处于"无法重建"状态**（生成文件过期 + 语法文件有 `$n` 越界
bug），且类型定义是 5 个硬编码关键字 + 两套并行枚举。要支持 sql_types 新增的
类型，改动集中在四处：`sql.l` 词法、`sql.y` 类型规则、`ast.h` 的
`ColumnDefNode`、以及**目前根本不存在的** AST→sql_types 桥接层。

---

## 一、P0：先让 parser 能构建（与类型无关，但不修没法继续）

| # | 问题                                                                                                                                                                                          | 证据                                                                                                                                                                                                                                                            | 状态                                                                                                                                                                                              |
| - | --------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- | --------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- | ------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| 1 | `sql.y:297` `$n` 越界：`$$ = append_to_list($1, $2)` 里的 `$2` 是逗号 token，应为 `$3`                                                                                              | Bison 3.8.2 报`error: $2 of 'order_list' has no declared type`，整个语法文件无法生成；Bison 2.3 不做类型检查所以当年能过，但生成的解析器在 `ORDER BY a, b` 时会把逗号当指针塞进链表（UB/崩溃）。改成 `$3` 后 `ORDER BY a, b` / `a DESC, b ASC` 均正常 | [Fixed] 09-11 已改为`$3`                                                                                                                                                                        |
| 2 | 生成文件过期且与源码同目录：`parser/parser.tab.h`（09-06）里还是 `OpType op;` / `DataType`，而 `sql.y`（09-08）已改为 `COpType` / `CDataType`                                     | `parser/parser.tab.h:118: OpType op;`；因 `#include "parser.tab.h"` 的引号包含优先取**同目录**，编译 `parser/parser.cpp` 直接报 `unknown type name 'OpType'`                                                                                      | [Partial] 09-11 已重新生成（`parser.tab.h` 现为 `COpType`），但**结构问题仍在**：生成物放在源码目录、被 `#include "parser.tab.h"` 优先命中，且没有"生成物比 `sql.l/sql.y` 新"的检查 |
| 3 | 工具链与构建没接上：`/usr/bin/bison` 是 2.3（连 `%code` 都不认），需要 ≥3.0；CMake **完全不构建 parser**，`tests/test_parser.cpp`、`test_ast.cpp` 只在 Makefile 里、没进 ctest | `make` 用的 `/usr/local/opt/bison/bin/bison` 是 3.8.2（本机可用）；`tests/CMakeLists.txt` 里 test_parser 是注释掉的                                                                                                                                       | [ ]                                                                                                                                                                                               |

建议：

- 生成物统一输出到 `build/`（`bison -o build/parser.tab.c` + `-Ibuild`），
  加"生成物必须比 `sql.l/sql.y` 新"的构建检查；

  - [Todo] 改， 注意配置bison路径：`/usr/local/opt/bison/bin/bison`。
- CMake 里加 flex/bison 的 custom command 并把parser 测试挂到 ctest。

  - [Todo] 改，并且将parser模块和测试目录tests/test_parser都加入到cmake中。
- `parser/parser.output` 是生成物却被 tracked，建议移出版本控制；

  - [Fixed] 已经修改git移除。
- 

---

## 二、当前能力矩阵（实测）

| SQL                                                   | 结果                                   | 说明                                                   |
| ----------------------------------------------------- | -------------------------------------- | ------------------------------------------------------ |
| `CREATE TABLE t (id INT PRIMARY KEY, name VARCHAR)` | ✅                                     | 现有能力上限                                           |
| `name VARCHAR(32)`                                  | ❌                                     | 没有长度语法                                           |
| `id TINYINT` / `id SMALLINT`                      | ❌                                     | 没有关键字（`BIGINT` ✅ 有）                         |
| `d DATE` / `d DATETIME`                           | ❌                                     | 同上                                                   |
| `c CHAR(8)`                                         | ❌                                     | 类型不存在                                             |
| `INSERT INTO t VALUES (NULL)`                       | ❌                                     | `TOK_NULL` 不是合法 `value_item`                   |
| `WHERE id IN (1, NULL)`                             | ❌                                     | 同上                                                   |
| `INSERT INTO t VALUES (3000000000)`                 | ⚠️ 解析"成功"但值变成`-1294967296` | `atoi` 截断（`atoi("99999999999999999999") = -1`） |
| `INSERT INTO t VALUES (-5)`                         | ⚠️ 解析"成功"但值变成`5`           | `-` 落到兜底规则被静默丢弃                           |
| `SELECT * FROM "t"`                                 | ⚠️ 解析"成功"但引号被丢弃            | 同上                                                   |
| `DELETE FROM t WHERE name IS NULL`                  | ✅                                     | `IS [NOT] NULL` 已支持                               |
| `WHERE name LIKE 'a%'`                              | ✅                                     | LIKE 已支持                                            |

---

## 三、类型链路的改造清单

### 3.1 `sql.l`（词法）

- [X] 补类型关键字/别名：`tinyint|int8`、`smallint|int16`、`int32`、`int64`、
  `char|character`、`date`、`time`、`datetime|timestamp`
  （现有：`sql.l:51-58` 只有 int/integer/bigint/varchar/text/boolean/bool）。

 	[Todo] 改。

- [X] **建议改成一个 `TOK_TYPE_NAME`**（携带 `"VARCHAR(32)"` 原文），由 C++ 侧
  `sql::parse_type_with_length()` / `string_to_data_type()` 做唯一定义与校验。
  原因：类型信息现在重复在 4 处（`sql.l` 关键字、`sql.y:145` 的 `data_type`、
  `common/c_types.h` 的 `CDataType`、`sql_types::DataType`），每加一个类型要改
  4 处且已经漂了——`common/c_types.h` 里已补的
  `DT_TINYINT/DT_SMALLINT/DT_CHAR/DT_DATE/DT_TIME/DT_DATETIME`
  **没有任何 parser 侧代码使用**。
  若保留独立 token，则至少保证"别名表只有一处"。

    [Todo] 同意修改。现在的问题初衷是我希望sql表示层 和 C风格的parser模块解耦。所以基本定义各搞各的。你也可以思考下这种逻辑下如何重构合适，最好还是能够只维护一到两份定义。

- [X] 数字字面量改 int64：`sql.l:83` 的 `atoi` + `%union { int num; }`
  让 BIGINT 列写不了大数；改为 `int64_t`（或 `yylval.str` +
  `sql::parse_int64_strict()`），`LIMIT/OFFSET` 用同一 token 需显式收窄并校验非负。

	[Todo] 改。

- [X] 负数：在语法层加一元负号（`'-' value_item`）；不要做在词法里。	

	[Todo] 改。

- [X] 兜底规则 `sql.l:96` 不能只 `fprintf` 后继续，否则未知字符被静默吞掉并让语句
  "解析成功"；应 `yyerror(...)` / 返回非法 token。[Todo] 改。
- [X] 字符串字面量 `'[^']*'` 不支持 `''` 转义（实测 `'it''s'` 失败），也不支持反斜杠
  转义；建议 `'([^']|'')*'` + 动作里还原；同时决定双引号标识符语义。[Todo] 改。
- [X] 行号永远是 1：`sql.l` 没有 `%option yylineno`，而 `parser/parser.cpp:38` 的
  `yyerror` 用 `yylineno` 拼信息（实测第 4 行错误报成 `line 1`）。[Todo] 改。
- [X] `strdup` 的 token 字符串无人释放（`make_column_def_node` 又 strdup 一次），
  每条 SQL 泄漏若干次；词法侧不 strdup（`yylval.str = yytext` + 动作拷贝）或统一 arena。[Fixed] 按lex/bison的说明，必须要strdup。后续在statement模块构建好C++版本的statement后，会调用free_ast来释放ast树，目前不用考虑该模块，只要fee_ast()实现正确就可以成功释放。

### 3.2 `sql.y`（语法）

- [X] `data_type`（`sql.y:145`）扩成"类型 + 可选长度"。`%union` 里放 POD struct
  （Bison 3.8 允许，非 POD 不行）：
  `%union { ... struct { CDataType type; uint32_t length; int valid; } spec; }`[Todo] 改。
- [X] 长度合法性**不要重新实现一遍**，复用 `sql::valid_declared_length()` /
  `can_represent_length()`（已实现并带测试：CHAR ≤255、VARCHAR ≤65535、
  TEXT 不接受长度、非字符串不接受长度）；或把类型原文存进 AST，交给 builder
  用 `TableSchema::add_column()` 校验（`INVALID_COLUMN_DEF`）。 [Todo] 改。
- [X] `value_item`（`sql.y:184`）加 `TOK_NULL` → `Value()`。注意 sql_types 里
  **NULL 与空串是两回事**，AST 不能都用空字符串表示。[Todo] 改。
- [X] 时间/布尔字面量：最省事是"字符串 + 列类型"（builder 用已有的
  `Value::from_string(str, col.type)`）；若要 `DATE '2024-01-01'`，需带类型的字面量节点。 [Todo] 改。
- [X] 删掉 `sql.y:196` 动作里的 `printf("DEBUG: assignment: ...")`。[Todo] 改。
- [X] 加 `%define parse.error detailed`，`yyerror` 带上当前 token 文本
  （现在只报 `syntax error`）。[Todo] 改。
- [X] `order_list` 补分号（Bison 3.8 容忍缺分号，但可读性/健壮性差）。[Todo] 需要分号吗？因为后面可能还有limit子句，这是同在一个select语句中的，尾部自带分号。

### 3.3 `ast.h` / `ast.cpp`

- [X] `ColumnDefNode`（`ast.h:309`）加 `length` 字段，与 `sql::ColumnDef`
  （`{name, type, length, nullable, primary_key}`）对齐；
  `make/print/free_column_def_node` 三处同步（打印要输出 `VARCHAR(32)`）。 [Todo] 改。
- [X] `NumberNode.value`（`ast.h:90`）`int` → `int64_t`，`print_number_node` 同步。[Todo] 改。
- [X] 加 NULL 字面量节点（如 `NODE_NULL_LITERAL`），用于区分 `NULL` 与 `''`。 [Todo] 改。
- [X] `ast.cpp:958` 的 `NodeLifetime nodes[]` 用 C99 designated initializer 且按下标
  与 `NodeType` 顺序绑定（C++ 编译有 `-Wc99-designator`）；建议改 `switch`
  或 `std::array<NodeLifetime, NODE_TYPE_COUNT>` + 静态断言——加 `NODE_NULL_LITERAL`
  时最容易错位。[Todo] 这里的争议是， ast.cpp是按C风格实现的，不应该加C++的东西。
- [X] **保持 AST 是 C**（`extern "C"` + POD），不要把 sql_types 的 C++ 类型塞进来；
  映射放在 C++ 侧。 [Todo] 改。同意，这也是类型搞了一份c风格的原因。

### 3.4 最关键的缺口：AST → sql_types 的桥不存在

- [X] `statement/statement_builder.cpp` **在仓库里不存在**（只有 `.h`，README 却写了它），
  AST→Statement/`sql::Query` 这条链是断的：类型支持做得再全也没人消费。[Fixed] 后续修改statement模块，暂时不用管。
- [X] `CompareOp`（sql_types）与 `COpType`（C API）之间**没有任何互转函数**，
  `statement_builder.h:55` 只有 `CompareOp get_compare_op(const char* op)`
  —— 第三套基于字符串的映射。建议在 `sql_types/compare_op.h` 补
  `COpType to_c(CompareOp)` / `CompareOp from_c(COpType)`，与
  `DataType↔CDataType` 保持一致，然后删掉字符串映射。[Fixed] 后续修改statement模块添加，暂时不用管。
- [X] 明确单向链路，且只在一处做类型校验：
  `sql.l/sql.y（语法）→ AST（枚举/原文 + length）→ ast_to_sql_types（C++）→ sql::ColumnDef → TableSchema::add_column/validate（语义校验）→ 存储`。[Fixed] 是的，同意这个顺序。后续修改statement模块，构建语句时校验类型，暂时不用管。

---

## 四、其他问题（与类型无关，但同源）

- [X] `parser/parser.cpp:91`、`Parser` 构造/析构都往 stdout 打日志
  （`[Parser] Parse successful` 等），`yyerror` 里 `fprintf` 同理；
  与 sql_types 已确立的"库代码不打日志"原则冲突。[Todo] 应该怎么改？
- [X] 语法覆盖限制（建议写进 README 支持矩阵）：只支持内联 `PRIMARY KEY`
  （无表级 `PRIMARY KEY (a,b)`）、单表 FROM（无 JOIN）、无 `IF NOT EXISTS`、
  无 `DEFAULT`、无 `DISTINCT`。[Todo] 同意修改readme

---

## 五、测试与工程

- [X] parser 测试接入 CMake/ctest（现在只在 Makefile，且需要 flex/bison ≥3）。

[Todo] 是的。老项目用Makefile, 需要修改cmake文件。

- [X] 新增类型相关用例：每个类型 + 别名的 `CREATE TABLE`；
  `VARCHAR(0)` / `CHAR(300)` / `VARCHAR(65536)` / `TEXT(10)` 的**解析期**报错；
  `>INT64_MAX` 字面量报错；`NULL` / `NOT NULL` / `PRIMARY KEY` 组合；
  `IS NULL` / `IN (…, NULL)`；负数；`''` 转义；多行输入的行号。

[Todo] 改，放到tests/test_parser目录

- [X] 表驱动测试"类型名/别名 → DataType + length"，断言 parser 与
  `sql_types` 的别名表一致（防止两处漂移）。

	[Todo] 改。

---

## 六、建议的实施顺序

1. **P0**：`$2→$3`；生成物移出源码目录 + 新鲜度检查；CMake 接 flex/bison 并把
   `test_parser` 挂到 ctest。目标：`cmake --build build` 能构建 parser，`ctest` 能跑。

	[Todo] 同意，改。

1. **字面量底座**：int64 数字、一元负号、NULL 字面量、未知字符报错、
   `%option yylineno`、去掉库内 printf。这一层不依赖类型改造，但会掩盖类型 bug。

	[Todo] 同意，改。

1. **类型入口统一**：`TOK_TYPE_NAME` + `parse_type_with_length()`；
   `sql.y` 支持 `VARCHAR(n)/CHAR(n)`；AST 加 `length`、`NumberNode` 改 int64、
   加 NULL 节点。[Todo] 需要修改，但是要考虑到parser模块偏向C，比较底层，且和sql_types表示层模块的独立性，是否要独立实现，或者交给sql表示层构建statement时校验更好，ast层不考虑，只是正确解析即可？
2. **桥接层落地**：`COpType↔CompareOp` 转换；实现 AST→`sql::ColumnDef`/`TableSchema`
   映射（可落 `statement_builder.cpp`），语义校验全部委托 sql_types。[Todo] 同意，但是statement模块后续再改，现在专注parser模块本身。
3. **体验项**：时间/布尔字面量、`''` 转义、双引号标识符、`parse.error detailed`。[Todo] 同意，改。
4. **文档与测试**：类型别名表驱动测试 + README 写明
   "类型名 ↔ DataType ↔ CDataType"的唯一映射链与各层职责。[Todo] 同意，改。

---

## 附：复现命令（全部在 /tmp 副本上操作，不写仓库）

```bash
# 1) 用现代 Bison 重新生成 -> 复现 $n 越界报错
/usr/local/opt/bison/bin/bison -d -o /tmp/gen/parser.tab.c parser/sql.y
#   parser/sql.y:297.62-63: error: $2 of 'order_list' has no declared type

# 2) 确认生成物过期（09-06 的 tab.h 仍是 OpType/DataType，sql.y 是 09-08）
grep -n 'OpType' parser/parser.tab.h      # -> 118:    OpType op;

# 3) 修好副本后重新生成，跑能力矩阵
mkdir -p /tmp/gen
sed -e '297s/append_to_list($1, $2)/append_to_list($1, $3)/' parser/sql.y > /tmp/gen/sql.y
/usr/local/opt/bison/bin/bison -d -o /tmp/gen/parser.tab.c /tmp/gen/sql.y
flex --header-file=/tmp/gen/lex.yy.h --outfile=/tmp/gen/lex.yy.c parser/sql.l
#   注意：parser/parser.cpp 的 #include "parser.tab.h" 会优先取同目录的过期头文件，
#   所以编译时要把 parser.cpp 复制到 /tmp/gen 一起编，或先删掉源码目录里的生成物。
```

（本文档所在的 `tests/test_parser/` 目录被 `.gitignore` 覆盖，不会被提交，
与 `tests/test_sql_types/codex_check_issues.md` 的处理方式一致。）

> 注：`.gitignore` 里原来的 `test_parser` 规则会连 `tests/test_parser/` 一起忽略，
> 已改为 `/test_parser`（只忽略仓库根目录下的可执行文件）。

---

# 2026-09-11 实施记录

验证：干净目录 `cmake` 配置 + 编译 + `ctest` 通过，0 告警；
`test_sql_types` 188 用例、`test_parser` 20 用例全部通过。

## 状态

| 条目 | 状态 | 说明 |
|------|------|------|
| P0-1 `$2` 越界 | [Fixed] | 已改为 `$3`（你已修） |
| P0-2 生成物位置/过期 | [Fixed] | CMake 生成到 `build/parser/`；源码目录里的生成物已删除；`parser.cpp` 改用 `#include <lex.yy.h>`/`<parser.tab.h>`（尖括号只查 `-I` 路径），对源码目录里的残留文件免疫 |
| P0-3 工具链/CMake 接入 | [Fixed] | `CMakeLists.txt` 增加 `find_package(BISON 3.0 REQUIRED)`（默认 `/usr/local/opt/bison/bin/bison`）+ `find_package(FLEX REQUIRED)`、`sql_parser_lib` 目标、`enable_testing()`；`tests/test_parser/` 接入 ctest |
| 3.1 类型关键字/别名 | [Fixed] | 不再逐个类型写关键字规则：lexer 查 `common/c_types.h` 的别名表，命中即返回 `TOK_TYPE_NAME` |
| 3.1 类型定义唯一数据源 | [Fixed] | 别名表 + 长度上限 + `c_type_lookup/c_type_length_valid` 都在 `common/c_types.h`；`sql_types::string_to_data_type`/`valid_declared_length`/`kMax*Length` 改为引用同一份 |
| 3.1 数字字面量 int64 | [Fixed] | `strtoll` + ERANGE 检查；AST `NumberNode.value` 改 `int64_t`；越界报 `integer literal out of range` |
| 3.1 负数 | [Fixed] | 语法层 `'-' TOK_NUMBER`（`-5` 不再被吞成 `5`） |
| 3.1 兜底规则 | [Fixed] | `.` 返回 `yytext[0]` 交给语法层报错（`"t"`、`#` 不再是静默忽略） |
| 3.1 字符串 `''` 转义 | [Fixed] | `'([^']|[']{2})*'` + 还原函数；`'it''s'` → `it's`。反斜杠转义与双引号标识符仍不支持（会报语法错误，已写进 parser/README） |
| 3.1 `%option yylineno` | [Fixed] | 行号正确（第 4 行报 `line 4`） |
| 3.1 strdup 泄漏 | [Fixed] | 按你的说明保留 `strdup`，由 `free_ast` 负责释放 |
| 3.2 `data_type` + 长度 | [Fixed] | `%union` 增加 `CTypeSpec{CDataType type; unsigned length; int has_length;}`；`VARCHAR(n)`/`CHAR(n)` 解析并填入 `ColumnDefNode.length` |
| 3.2 长度合法性复用 | [Fixed] | 解析期用 `c_type_accepts_length()` / `c_type_max_length()`（与 sql_types 同一份常量）：`CHAR(300)`/`VARCHAR(0)`/`VARCHAR(65536)`/`TEXT(10)`/`INT(4)` 直接报错 |
| 3.2 `TOK_NULL` 作值 | [Fixed] | `value_item` 增加 `NULL`/`TRUE`/`FALSE`（新节点 `NODE_LITERAL`，**NULL 与空串是不同节点**），`IN (1, NULL)` 也可用 |
| 3.2 时间/布尔字面量 | [Fixed] | 时间走"字符串 + 列类型"（builder 后续用 `Value::from_string`）；布尔新增 `TRUE`/`FALSE` 关键字字面量 |
| 3.2 删 DEBUG printf | [Fixed] | 已删 |
| 3.2 `parse.error detailed` | [Fixed] | 已加；并且 `yyerror` 现在**只记录第一个错误**，词法层的具体错误不会被 `syntax error` 覆盖 |
| 3.2 `order_list` 分号 | [Fixed] | 已补。回答你的疑问：Bison 3.8 容忍缺分号，它与 SQL 语句结尾的分号无关（后者由 `input: statement ';'` 处理），补上纯粹为了可读性 |
| 3.3 `ColumnDefNode.length` | [Fixed] | 已加，`make/print/free` 同步；打印输出 `VARCHAR(32)` |
| 3.3 `NumberNode` int64 | [Fixed] | 已改 |
| 3.3 NULL 字面量节点 | [Fixed] | 新增 `NODE_LITERAL`（`LITERAL_NULL/TRUE/FALSE`），避免用空串表示 NULL |
| 3.3 NodeLifetime 表 | [Fixed] | 采用纯 C 方案：去掉指定下标初始化、按 `NodeType` 顺序排列，并加 `typedef char[...]` 的编译期长度检查（未引入任何 C++ 设施） |
| 3.3 保持 AST 为 C | [Fixed] | AST 仍为 `extern "C"` + POD；生成代码**按 C 编译**（`parser.tab.c`/`lex.yy.c`），`parser.cpp` 用 `extern "C" { #include <...> }` 引入生成头 |
| 3.4 AST→sql_types 桥 | 按你的意见延后 | statement 模块后续实现，本模块只保证"解析正确" |
| 四、库内打印 | [Fixed] | 库默认不打印；新增 `Parser::set_log_callback()`（`main.cpp` 未用 `Parser` 类，CLI 行为不变） |
| 四、支持矩阵写进 README | [Fixed] | 新增 `parser/README.md`（职责边界、唯一数据源、构建、支持/不支持矩阵、错误处理约定） |
| 五、测试接入 CMake/ctest | [Fixed] | `tests/test_parser/`（main + 3 个测试文件）挂到 ctest |
| 五、新增类型用例 | [Fixed] | 20 个用例：所有别名表驱动、`CHAR(n)/VARCHAR(n)` 边界、`NULL/TRUE/FALSE`、负数、int64 越界、`''` 转义、`IN (1,NULL)`、LIMIT 范围、行号、未知字符不再被吞 |
| 五、别名表一致性测试 | [Fixed] | `TypeAliasTablesStayInSync`：遍历 C 别名表断言 `sql_types::string_to_data_type` 与 `to_c/from_c` 一致 |

## 遗留（本轮有意不做）

- statement 模块：AST → `sql::ColumnDef`/`Value`/`Query` 的映射与语义校验。
- 双引号/反引号标识符、字符串反斜杠转义。
- 表级约束、`DEFAULT`、`IF [NOT] EXISTS`、JOIN/子查询/`DISTINCT`/`BETWEEN`。
- 旧的 `tests/test_parser.cpp`、`tests/test_ast.cpp` 仍是 Makefile 专用；
  二者已随 AST 改动同步（`make_column_def_node` 增加 length 参数）。

## 复现命令

```bash
cmake -S . -B build -DCMAKE_C_COMPILER=clang-mp-23 -DCMAKE_CXX_COMPILER=clang++-mp-23
cmake --build build -j4
ctest --test-dir build --output-on-failure      # test_sql_types / test_parser
```

---

# 2026-09-11 旧测试迁移

把两个老的、Makefile 专用的 parser 测试程序搬进 `tests/test_parser/` 并删除原文件：

| 旧文件 | 新位置 | 说明 |
|--------|--------|------|
| `tests/test_ast.cpp`（576 行，12 组） | `tests/test_parser/test_ast_nodes.cpp`（12 个 TEST） | 基础节点、列表、SELECT/INSERT/UPDATE/DELETE、条件节点、DDL、ORDER/LIMIT、NodeLifetime 注册表、复杂 AST 递归释放、print_ast 冒烟 |
| `tests/test_parser.cpp`（454 行，14 组） | `tests/test_parser/test_sql_statements.cpp`（15 个 TEST） | 基础 SELECT、复杂条件/括号、IN/LIKE、IS NULL、ORDER BY/LIMIT、INSERT/UPDATE/DELETE、DDL、错误 SQL、复杂组合、重复解析状态重置、C API 路径、ParseResult/parse_multi |

移植要点：

- 断言从自制的 `TEST_ASSERT`/printf 换成 `test_fwk` 的 `TEST`/`CHECK`，输出统一；
  原有的"打印 AST 看结构"改为断言节点字段（更严格），只保留 `print_ast` 一次冒烟。
- 补齐旧测试没覆盖的新能力：`NODE_LITERAL`（NULL/TRUE/FALSE）、
  `ColumnDefNode.length`（打印含 `VARCHAR(32)`）、`IN (1, NULL)`、`<>`。
- **移植中发现一个真实缺口**：`sql.l` 只有 `!=` 规则，没有 `<>`
  （README 却把 `<>` 列入支持），已补 `"<>" { return TOK_NE; }`。
- 旧 `test_parser.cpp` 把 `UPDATE users SET age = age + 1 ...` 当作**应成功**，
  但语法并不支持算术表达式：现在测试显式断言它被拒绝，
  并在 `parser/README.md` 的支持矩阵里标注"不支持算术表达式"。
- `Makefile` 里对应的 3 处引用（`TESTS_SRCS`、`TEST_TARGETS`、`.o` 规则与链接目标、
  `test-quick`）已移除；新增 `make parser-test` 与 `make cmake-test`（走 ctest）。

结果：`test_parser` **55 用例 / 491 断言 / 0 失败**，
`test_sql_types` 188 用例 / 1159 断言 / 0 失败，`ctest` 2/2 通过。

---

# 2026-09-12 高亮改为复用 lexer 的 token 流（方案 B）

背景：高亮器原来自己写了一套扫描器（+ 一份硬编码关键字表），存在**双向漂移**：

- `ASCENDING`/`DESCENDING` 由 `sql.l` 认作关键字，但高亮器表里没有 → 不着色；
- 高亮器把 `--` 当注释，而 `sql.l` 根本没有注释规则 → 把语法错误位置显示成注释。

方案（确认采纳 B）：**词法规则只留在 `sql.l`**，高亮器退化为"token → 颜色"映射。

## 实现

1. 新增 `parser/lex_tokens.h`：`LexToken{kind, span, offset, length}` +
   `lex_collect_tokens(sql, &out)`（返回 token 数，数组由 malloc 分配、调用方 free）。
2. `parser/sql.l`：
   - 用 `#define YY_DECL int lex_yylex(void)` 把 flex 生成的扫描函数改名，
     另加 `int yylex(void) { return lex_yylex(); }` 作为解析器入口（解析路径不变）；
   - 用户代码段实现收集器：把 SQL 拷进带两个 EOB 的可变缓冲 → `yy_scan_buffer`
     （in-place，可用指针差算 offset）→ 循环 `lex_yylex()` 收集 → 清理；
     空白/注释不产生 token，由调用方用 offset 空隙还原原文。
3. `statement/sql_highlight.cpp` 重写：消费 token 流，按 kind 上色；
   删除了手写扫描器与关键字表、`upper_ascii/is_keyword/is_type_name` 等辅助函数。
   - **没有关键字清单**：凡 `kind >= 258`（Bison 用户 token）且不属于
     NUMBER/STRING/TYPE_NAME/IDENT/NULL/TRUE/FALSE/比较运算符的特例，一律按关键字上色。
   - 分类只依赖 token id，所以 `sql.l` 加关键字时高亮自动跟随。
4. 保留 `highlight_span()`（按 span 标红）与 `sspan_caret()`（错误定位 + caret）不变。

## 测试

- `tests/test_parser/test_lex_tokens.cpp`（6 个用例）：kind/offset/span 正确、
  空白跳过但 offset 连续、空输入 0 token、字符串/数字/字面量原文切片、
  未知字符是单字符 token、类型名报 `TOK_TYPE_NAME`。
- `tests/test_statement/test_highlight.cpp` 新增 3 个用例：
  `ASCENDING/DESCENDING` 现在被当作关键字着色（漂移消除的回归）、
  `--` 与 lexer 行为一致（不当注释）、8 组刁钻输入"去色后逐字节等于原文"。

结果：`test_parser` 63 → **69 用例**，`test_statement` 48 → **51 用例**，
`test_sql_types` 209 用例；干净目录 configure+build+ctest 3/3 通过、0 告警。

## 后续

- 若将来给 `sql.l` 加注释/浮点/双引号标识符规则，高亮**自动**跟随
  （`CommentLikeTextFollowsLexerNotIntuition` 这条用例的作用是提示：
  加了注释规则后该用例的期望需要相应更新）。
- `lex_collect_tokens()` 与解析器一样非重入，且不要在 `yyparse()` 进行中调用
  （已在 `lex_tokens.h` 与 README 注明）。

---

## 事务控制语句进语法层（BEGIN / COMMIT / ROLLBACK）

背景：`BEGIN/COMMIT/ROLLBACK` 之前是 **session 层的文本匹配**
（`session.cpp` 的 `transaction_command()`），语法层完全不认识它们。
只要关键字由上层"逐字符比对"识别，就会出现与高亮器同样的漂移：
注释、大小写、`EXPLAIN BEGIN` 这类组合绕过词法/语法检查，
别名（`START TRANSACTION`/`END`/`ABORT`）也没有统一出处。

改动（只碰 parser + 上层分派，不动别的语法）：

| 位置 | 内容 |
|------|------|
| `sql.l` | 8 个新关键字规则：`begin/start/transaction/work/commit/end/rollback/abort`（整词、大小写不敏感） |
| `sql.y` | `transaction_stmt` 产生式 → `NODE_TRANSACTION`；别名归一化成 `TXN_BEGIN/TXN_COMMIT/TXN_ROLLBACK`；不支持的形式（`AND CHAIN`、`ROLLBACK TO SAVEPOINT`、事务模式/隔离级别）调用 `yyerror()` 后 `YYERROR`，给出**具体原因**而不是 `syntax error` |
| `ast.h/.cpp` | `NODE_TRANSACTION` 放在 `NODE_DROP_TABLE` 之后、`NODE_TYPE_COUNT` 之前（枚举末尾不能挪，注册表按序索引）；`TransactionNode{kind}` + make/print/free + 注册表一项 |

注意点：

* **保留字代价**：`TRANSACTION`/`WORK` 原本能当标识符，现在也被保留
  （`BEGIN/START/COMMIT/END/ROLLBACK/ABORT` 在标准 SQL 里本来就是保留字）。
  要放宽的话得走"非保留关键字"那套（lexer 返回 IDENT + 语法层按文本判定），
  目前不值得。
* **高亮自动跟随**：`statement/sql_highlight.cpp` 没有关键字清单，
  `kind >= 258` 一律按关键字上色，所以新 token 不需要改它；
  `test_transaction.cpp` 里有一条用例直接断言 token 流里是
  `TOK_START`/`TOK_TRANSACTION`（而不是被当成 IDENT）。
* 事务语句的执行、会话状态（重复 BEGIN、"事务已中止"）仍在 session —— 语法层
  只回答"这是哪一种操作"。

## EXPLAIN 进语法层（语句前缀 + 结果集）

`EXPLAIN`/`ANALYZE` 同样进了 `sql.l`/`sql.y`：

| 位置 | 内容 |
|------|------|
| `sql.l` | 关键字 `explain` / `analyze` |
| `sql.y` | `explain_stmt: TOK_EXPLAIN [TOK_ANALYZE] statement` → `NODE_EXPLAIN{analyze, statement}` |
| `ast.h/.cpp` | `NODE_EXPLAIN`（前缀节点，free 递归释放内层）+ 注册表 |

要点：

* **前缀节点，不是包装查询**：内层还是原来的节点类型（`NODE_SELECT` /
  `NODE_TRANSACTION` ...），session 拆掉这层之后照常走
  builder/validator/planner；`sql::Query` 里**没有** EXPLAIN ——
  否则 Query 变体要自引用（`unique_ptr<Query>`），代价不值得。
* 位置全程是原文列号：`EXPLAIN SELECT * FROM missing` 只在 `missing`
  处报错并高亮，不再需要"把列号换算回原文"那套。
* `EXPLAIN BEGIN` 能被语法层包住（不报语法错），由 session 报 NOT_SUPPORTED。
* 保留字：`EXPLAIN`/`ANALYZE` 从此不能当标识符。

新增用例：`tests/test_parser/test_explain.cpp`（5 条）——
前缀包住语句、`ANALYZE` 标志、六种内层语句、大小写/空白、token 流、
`EXPLAIN;` / `ANALYZE ...` / `EXPLAINX` 的报错。
test_parser：77 → **82 用例**。

## 多行 VALUES：`values` 统一成"行的列表"

`insert_values` 改成左递归的两条规则（单行 = 行数为 1 的列表）：

```
insert_values:
    '(' value_item_list ')'                     { $$ = create_list($2, LIST_VALUE); }
    | insert_values ',' '(' value_item_list ')' { $$ = append_to_list($1, $4); }
```

如果保留"单行扁平 + 另加 row_list"，`)` 之后遇到 `;` 会出现 **reduce/reduce
冲突**（bison 按规则顺序硬选，语义靠运气）—— 统一成"行的列表"就没这个问题，
与 builder 里早就写好的多行分支（`values->head->type == NODE_LIST`）也对上了。

代价：解析出来的单行 INSERT 多一层（`values -> row`），
`test_literals.cpp` 取"第一个值"的小工具改了两行；
`make_insert_node` 手工构造扁平 AST 的用法不受影响（builder 两条路径都收）。

新增用例（`test_sql_statements.cpp` +2）：多行解析、AST 形状（行数 × 每行值数、
值本身）、逗号写错报语法错。test_parser：82 → **84 用例**。

## P0：多线程（服务端）下的解析安全

为协程服务器做准备时发现：**parser 是进程级单例状态**，多线程直接崩。

### 1. 证据（先写测试，跑在**未修**的代码上）

`tests/test_parser/test_thread_safety.cpp`（8 线程 × 300 次 × 2 个用例）：

```
$ ./build/run_tests/test_parser        # 未修版本
fatal flex scanner internal error--end of buffer missed   ← 每次必崩
```

两个用例的判据也不同角度：① 每个线程解析 `SELECT ... FROM t<N> ...`，
断言拿到的 AST 指向**自己的**表名（串状态就会抓到）；② 一半线程一直解析
坏语句、一半解析好语句，好语句不能因为别人的语法错误而失败。

### 2. 两处竞态（TSan 抓的第二处是我一开始漏掉的）

| # | 竞态 | 修法 |
|---|------|------|
| 1 | flex 的扫描缓冲/`yytext`/`yylloc`/`yylineno`、`ast.cpp` 的 `g_parsed_ast`、`parser.cpp` 的 `g_parser_state` 全是进程级全局 | `Parser::parse()` 里一把进程级锁，把"登记错误接收方 → `yyparse` → 取走 AST → 摘掉接收方"整段包起来；`g_parser_state.instance` 只在解析期间登记（RAII），不再在 `Parser` 构造/析构里设 |
| 2 | `ast_debug`：解析线程**写**（`do_parse` 按 `set_debug()` 设），别的线程在 `free_ast` 的 `DEBUG()` 里**读**（TSan: `Write of size 4 ... Location is global 'ast_debug'`） | 改成 `std::atomic<int>`（读端用 `load(relaxed)`） |

顺带把 `Parser::reset()` 也纳入同一把锁（它同样改全局错误状态），
内部拆出不加锁的 `clear_error_state()` 给 `parse()` 用（避免自锁）。

### 3. 结果

- `test_parser` 84 → **86 用例**，多次重复运行稳定；`ctest` 11/11。
- **ThreadSanitizer 下 0 竞态**（`-fsanitize=thread` 单独 build 跑 test_parser）。
- 边界写进 `parser/README.md`（中英）：`parse()` 线程安全；
  **`lex_collect_tokens()`（高亮）仍非重入** → 服务端不做高亮，把 span 回给
  客户端渲染；M3 要真并行解析时按 README 里的"可重入化清单"改
  （bison `api.pure` + flex reentrant），再把锁删掉。

新增用例：`tests/test_parser/test_transaction.cpp`（7 条）——
四种 BEGIN 写法、`COMMIT|END`、`ROLLBACK|ABORT`、`WORK` 后缀、大小写、
节点打印与 span、不支持形式的报错文案、`BEGINNER` 不是关键字。
test_parser：70 → **77 用例**，`-Wall -Wextra` 0 告警。

---

## 关键字清单导出：`lex_keywords()`（给客户端 TAB 补全）

需求：客户端的自动补全要有**唯一数据源**，不能在 REPL 里再抄一份关键字
清单（两处维护必然漂移）。

### 实现

`parser/sql.l` 的用户代码段新增：

```c
static const char* const kLexKeywords[] = { "select", "from", ... , NULL };
int lex_keywords(const char* const** out);   /* 声明在 parser/lex_tokens.h */
```

和关键字**规则表**在同一个文件里——改关键字时"两张表一起改"。返回的数组是
静态的、以 `NULL` 结尾，调用方**不要 free**。补全侧消费点：
`client::completion_candidates()`（`client/repl.cpp`）。

### 为什么不做成"从 `sql.y` 的 `%token` 表自动导出"

`%token` 只有**符号名**（`TOK_SELECT`），拿不到**拼写**（`select`）；拼写只
存在于 `sql.l` 的规则里。所以清单只能和 `sql.l` 同源（这也是之前定的
"来源单一在 sql.l"）。

### 测试（防漂移）

`tests/test_parser/test_lex_tokens.cpp` 新增两条：

| 用例 | 断言 |
|------|------|
| `LexTokens.KeywordListMatchesTheLexerAndIsCaseInsensitive` | 清单里每个词：① 全是小写；② 不重复；③ 词法层必须识别成**关键字**（不是 `TOK_IDENT`/`TOK_TYPE_NAME`）；④ **全大写拼写落到同一个 token**（顺带把"大小写无关"钉死） |
| `LexTokens.KeywordListCoversCoreSqlVocabulary` | 核心词汇（select/from/where/insert/.../begin/commit/rollback/explain/analyze/null/in/is/like 等）必须都在清单里——防止有人删规则时顺手删清单里的词 |

这样"清单里多/少一个词"和"大小写不生效"两类问题都会在 `test_parser` 直接变红，
不用等客户端发现。`test_parser`：86 → **88 用例 / 2072 断言 / 0 失败**。
