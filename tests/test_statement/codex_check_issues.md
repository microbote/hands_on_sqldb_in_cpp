# statement 模块完善记录（StatementBuilder / StatementValidator）

审查范围：`statement/stmt_builder.h`、`statement/stmt_validator.h`、
`statement/stmt_defs.h`，以及它们依赖的 `sql_types/{query,catalog,schema}`、
`parser/ast.h`、`tests/test_statement_builder.cpp`（旧测试）。

结论：两个类按你写的接口补齐并跑通；过程中修掉 1 个真实缺陷
（PRIMARY KEY 未隐含 NOT NULL），并补了 2 处必需接口
（`Catalog::current_database()`、`CompareOp ↔ COpType` 桥）。

验证：干净目录 configure + build + ctest，0 告警，3/3 测试套件通过；
`test_statement` 28 用例 / 228 断言 / 0 失败。

---

## 一、接口现状（你写的骨架）

| 位置 | 问题 | 处理 |
|------|------|------|
| `stmt_builder.h:34` `build()` | 调用了 `build_stmt_use/select/insert/...`，但**一个都没声明** | 补齐 9 个 `build_stmt_*` + 片段转换方法 |
| `stmt_builder.h:43` | `NODE_UPDATE` 分支调用了 `build_stmt_insert`（笔误） | 改为 `build_stmt_update` |
| `stmt_builder.h:37/45/50` | `return StmtError::X`（`std::expected` 需要 `std::unexpected`） | 全部改为 `std::unexpected(...)`，并同时记录可读 `error_` |
| `stmt_builder.h:26-31` 析构 | `free_ast(ast_)` 会与调用方的 `ParseResult`/`free_ast` 双重释放，且 `ast_` 从未被赋值 | 明确 **builder 不接管 AST 所有权**，去掉该成员与释放逻辑 |
| `stmt_builder.h:43` 转义/命名 | 文件结尾注释写 `namespace query`（实际是 `namespace stmt`） | 注释更正 |
| `stmt_validator.h:12` | `catalog_.is_open()` 当时不存在 | 你已加 `is_open()`；本模块再补 `current_database()`（见下） |
| `stmt_validator.h:17` `validate()` | DDL/DML/CTRL 三条分支没有 return；`validate_dml` 只有 `default`；`validate_ctrl` 是空 switch | 三条分派补齐并全部返回 |
| `stmt_defs.h` | 只有 4 个错误码，无法表达校验失败原因 | 扩展为 校验类 1..49 / 构建类 99+，并加 `stmt_error_message()` |

结构调整：两个头文件保留为**接口**（类名、方法名、签名与原来一致），
实现移到 `statement/stmt_builder.cpp` / `statement/stmt_validator.cpp`
（与 sql_types/parser 的工程风格一致，也便于单测）。

## 二、实施内容

### StatementBuilder（AST → sql::Query）

| AST 节点 | 产物 |
|----------|------|
| `NODE_USE` | `sql::UseDatabaseQuery` |
| `NODE_SELECT` | `sql::SelectQuery`（列列表、WHERE、ORDER BY、LIMIT/OFFSET） |
| `NODE_INSERT` | `sql::InsertQuery`（列清单可空、VALUES 单行；代码已兼容"元素是列表"的多行形态） |
| `NODE_UPDATE` | `sql::UpdateQuery`（SET 列表、WHERE） |
| `NODE_DELETE` | `sql::DeleteQuery`（WHERE） |
| `NODE_CREATE_TABLE` | `sql::CreateTableQuery`（`ColumnDef` 含声明长度 `length`） |
| `NODE_DROP_TABLE` | `sql::DropTableQuery` |
| `NODE_CREATE_DATABASE` / `NODE_DROP_DATABASE` | `CreateDatabaseQuery` / `DropDatabaseQuery` |

值转换：`NODE_NUMBER`→整数、`NODE_STRING`→VARCHAR、`NODE_LITERAL`→
NULL/TRUE/FALSE（**NULL 与空串是两个不同的值**）。
条件树：COMPARE/IN/NOT IN/AND/OR/NOT → `sql::ConditionPtr`，
操作符通过 `sql::from_c(COpType)` 转换。

### StatementValidator（Query + Catalog → StmtError）

| 语句 | 规则 |
|------|------|
| `USE` / `DROP DATABASE` | 库必须存在 |
| `CREATE DATABASE` | 库不能已存在 |
| `CREATE TABLE` | 库存在、表不存在、schema 合法（有主键/无重复列/声明长度合法） |
| `DROP TABLE` | 库与表都存在 |
| `SELECT` | 表存在；SELECT 列表、WHERE、ORDER BY 引用的列都存在 |
| `INSERT` | 表存在；显式列清单的列存在；值个数一致；值类型族与范围匹配；NOT NULL 列必须有值 |
| `UPDATE` | 表存在；SET 列存在且类型匹配；主键列不允许更新；WHERE 引用的列存在 |
| `DELETE` | 表存在；WHERE 引用的列存在 |

列值校验**直接复用 `sql::TableSchema::validate_value()`**，
所以整数宽度（TINYINT 范围）、字符串声明长度（VARCHAR(32)）、
时间类型、NULL 约束的规则与 sql_types 完全一致，不存在两套规则。
条件树里的列用 `sql::ConditionVisitorBase` 遍历收集。

## 三、为此补充的接口（需要你确认）

1. **`sql::Catalog::current_database()`**（`sql_types/catalog.h`）
   SELECT/INSERT/UPDATE/DELETE 的 Query 不带库名，validator 必须知道当前库
   才能解析表；`DatabaseView` 的既有设计也表明"当前库"属于会话层。
   `tests/test_sql_types/test_catalog.cpp` 的内存实现已同步（并补 `is_open()`）。
2. **`CompareOp ↔ COpType` 桥**（`sql_types/compare_op.h` 的 `to_c/from_c`）
   上次 parser 审查里商定"放到 statement 阶段"；同时给 `COpType` 末尾追加
   `OP_NOT_LIKE`（既有枚举值不变），让桥接不丢信息。
   `StatementBuilder` 因此不需要任何字符串映射表。
3. **`StmtError` 扩展**（`statement/stmt_defs.h`）
   校验类 1..49、构建类 99+；原有 `OK/VALIDATOR_NOT_INIT/AST_IS_NULL/
   UNKNOWN_AST_Type` 数值未变。

## 四、语义决定（可以按需调整）

1. **PRIMARY KEY 隐含 NOT NULL**：在 builder 规范化
   （`column.nullable = nullable && !primary_key`）。
   测试发现 `CREATE TABLE t (id INT PRIMARY KEY)` 会被 sql_types 判
   "invalid row"，根因就是解析器给出的 `nullable=1`。
   如果你更希望放在语法层（`sql.y` 的 `opt_primary_key` 动作里），我可以平移。
2. **builder 不接管 AST 所有权**：AST 由解析方释放
   （`parser::ParseResult` 的 `ASTNodePtr`），否则和旧测试里的 `free_ast` 冲突。
3. **validator 只读**：校验通过 ≠ 已执行。`DROP TABLE users` 校验成功后表仍在，
   执行器需要自己调 `Catalog::drop_table()` 等。测试里专门固化了这个性质。
4. **UPDATE 不允许改主键列**（返回 `INVALID_SCHEMA`），避免行迁移；
   MySQL 允许，如需放开可以按配置开关处理。
5. **INSERT 显式列清单时，未提到的列必须可空**（暂无 DEFAULT 支持）；
   违反时返回 `COLUMN_ATTR_NULL_MISMATCH`。

## 五、测试与工程

新增 `tests/test_statement/`（走 ctest）：

| 文件 | 覆盖 |
|------|------|
| `test_builder.cpp`（12 用例） | null AST、SELECT/INSERT/UPDATE/DELETE/DDL 的字段级断言、条件树结构、`VARCHAR(32)` 长度保留、NULL/TRUE/FALSE/负数、AST 所有权不被接管 |
| `test_validator.cpp`（12 用例） | 每条校验规则的命中与放行、Catalog 关闭、无当前库、TINYINT 范围、只读语义（校验不执行 DDL） |
| `test_pipeline.cpp`（4 用例） | SQL 文本 → 解析 → 构建 → 校验 → 执行（模拟执行器把 DDL 落到 Catalog）的端到端 |
| `memory_catalog.h` | 测试用内存 Catalog（含 `is_open`/`current_database`） |

CMake：新增 `sql_statement` 静态库（依赖 `sql_types` + `sql_parser_lib`）
与 `tests/test_statement`；根 CMakeLists 里原有的 `enable_testing()` 继续复用。
Makefile：删除已死的 `test_statement` 目标与规则，新增 `make statement-test`。
`tests/test_statement_builder.cpp`（引用已删除的 `statement_builder.h`/`mock_engine.h`，
早已无法编译）已删除，其场景（`SELECT * FROM non_existent`、
`SELECT invalid_col FROM users` 等）并入 pipeline 测试。

## 六、遗留 / 后续

- 执行器侧：如何消费 `sql::Query`（DDL 落 Catalog、DML 生成扫描计划）。
- `UPDATE` 支持主键、`DEFAULT`、多行 `VALUES`、`SET a = a + 1` 表达式
  （最后一条依赖 parser 支持算术表达式，目前语法层就会拒绝）。
- 表别名/多表 JOIN/子查询/DISTINCT/GROUP BY 等语法本身尚未支持，
builder/validator 也只能等语法先行。
- `tests/test_query.cpp`、`tests/test_condition.cpp` 等旧测试仍引用已删除的
  statement API，目前不在 CMake 里；建议后续按同样方式迁移或删除。

## 复现命令

```bash
cmake -S . -B build -DCMAKE_C_COMPILER=clang-mp-23 -DCMAKE_CXX_COMPILER=clang++-mp-23
cmake --build build -j4
ctest --test-dir build --output-on-failure     # test_sql_types / test_parser / test_statement
make statement-test                            # 等价
```

---

# 2026-09-11 错误类型重构（回复"build() 的返回类型"）

问题（讨论）：`std::expected<sql::Query, StmtError>` 现代但"不全"——
错误信息只能通过对象的 `error_`/`error()` 取；
是否应该改成 `struct BuildOutcome { bool ok; StmtErrorCode error;
std::string message; std::optional<Query> query; }`？

**结论：不要用那个结构体，而是把错误本身做成"值"。**

## 为什么不用 BuildOutcome

1. 三个冗余状态：`ok`、`error`、`query.has_value()` 可以互相矛盾
   （`ok=true` + `error=TABLE_NOT_FOUND` 是合法可构造的），没有东西约束它们。
2. 它本质是手写的 `expected`：丢掉 `and_then/or_else/transform/value_or`、
   `operator bool`、`value()` 等设施。
3. 风格分裂：sql_types 已经在用 `std::expected`（RowBuilder、
   `Row::deserialize`、`TableSchema::deserialize`），再引入一套结构体
   会让三个模块三种写法。

## 而且旧设计已经真实出错（先复现、后修复）

原实现把消息放在对象成员 `error_` 里，`validate()` 开头又是
`if (has_error()) return VALIDATOR_NOT_INIT;` —— 于是
**同一个 validator 只要失败过一次，后续所有 validate() 都返回
VALIDATOR_NOT_INIT**。先加回归测试复现（当时输出 FAILED），再改设计修掉。

## 采用的方案

```cpp
enum class StmtErrorCode : uint8_t { OK = 0, ... , AST_IS_NULL = 99, ... };

struct StmtError {                    // 错误码 + 上下文信息，作为值返回
  StmtErrorCode code = StmtErrorCode::OK;
  std::string message;
  bool ok() const;
  explicit operator bool() const;
  std::string to_string() const;       // message 为空则退回通用描述
};

using BuildResult  = std::expected<sql::Query, StmtError>;         // builder
std::expected<void, StmtError> validate(const sql::Query&) const;  // validator
```

改动：

- `stmt_defs.h`：枚举改名 `StmtError` → `StmtErrorCode`，新增 `struct StmtError`；
  `stmt_error_message(StmtErrorCode)` 保留。
- `StatementBuilder`：删除 `error_`/`has_error()`/`error()`/`clear_error()`；
  所有失败点改为 `std::unexpected(StmtError{code, message})`（31 处），
  消息里带上下文（例如 `"unsupported value node: NODE_..."`）。
- `StatementValidator`：签名改为 `std::expected<void, StmtError>`；
  删除 `error_`/`has_error()`；"Catalog 未打开"从**构造期状态**改成
  **每次 validate() 重新判断**并返回 `VALIDATOR_NOT_INIT`；
  `load_table()` 改为返回 `expected<TableSchema, StmtError>`（表不存在时把
  库名/表名带进消息）。
- 两个类因此**完全无状态**：可重复调用、可并发使用。

副作用（正向）：`validate` 与 `build` 现在同构，可以写
`builder.build(ast).and_then(...)` 这类链式代码。

## 测试

- 新增 `Validator.ClosedCatalogIsRejectedOnEveryCall`（重复调用每次都返回
  VALIDATOR_NOT_INIT）与 `Validator.ReuseAfterFailureStillWorks`
  （第一次失败不影响第二次）——后者在改设计前是 FAILED，现在通过。
- 测试代码同步改用 `expected` 语义：`result.has_value()` / `result.error().code`
  / `result.error().message`；`test_validator.cpp`、`test_pipeline.cpp` 重写为
  新接口的示范用法。

结果：`test_statement` **29 用例 / 213 断言 / 0 失败**，
`test_sql_types` 188 用例、`test_parser` 55 用例，`ctest` 3/3 通过，0 告警。

---

# 2026-09-12 source_span（错误定位）

目标：让错误不只知道"哪条语句错了"，还能指出"**哪个名字/字面量错了**"。

## 位置链路（新增）

```
sql.l  %option yylineno + 自己维护列号（YY_USER_ACTION / lex_advance）
  │      yylloc{first_line, first_column, last_line, last_column}
  ▼
sql.y  %locations；语法动作里 AST_SET_SPAN($$, @$) / @n
  │      ASTNode::span；另有 SelectNode::table_span、
  │      Insert/Update/Delete/CreateTable/DropTable::table_span、DatabaseNode::db_span
  ▼
StmtError.span（值类型的一部分，随 expected 一起返回）
  │      builder：直接取失败节点/表名/库名的位置（并在 build() 里兜底为整条语句）
  │      validator：可选 SpanResolver，按名字（表/列/库）查位置
  ▼
sspan_caret(sql, span) → CLI 直接画出来
```

新增文件：`common/source_span.h`（C 兼容的 `SSpan` + 内联断言/工具）、
`statement/source_span.h/.cpp`（`sspan_to_string`、`sspan_caret`、
`collect_source_spans`、`make_span_resolver`）。

## API 变化

- `StmtError` 增加 `SSpan span`（未知位置为全 0，`sspan_valid()` 判断）；
  `to_string()` 会自动带上 `(line L:C)`；另有 `with_span()` 便于调用方补充。
- `StatementValidator(catalog)` → `StatementValidator(catalog, SpanResolver = {})`；
  不传 resolver 时行为与之前完全一致（span 未知）。
- `SSpan` 是"行/列（按字节）、列区间左闭右开"，跨行为 `line 2:3..4:5`。

## 顺带修掉的两个问题

1. **AST 构造器在 nullptr 名字上崩溃**：所有 `make_*_node` 直接 `strdup(name)`，
   传 NULL 会段错误（我写"缺表名"的测试时触发了它）。已改为统一的
   `ast_strdup()`（NULL 安全），16 处构造器全部替换。
2. 词法列号推进：`YY_USER_ACTION` 负责推进列号，`lex_advance()` 只修正
   token 内部的换行（字符串/空行），两者不重复计数。

## 测试

- `tests/test_parser/test_source_span.cpp`（8 个用例）：整条语句 span、表名 span、
  条件/字面量 span、列名列表 span、跨行（第二行第 8 列）、空行计数、
  列定义 span、多行语法错误的行号。
- `tests/test_statement/test_source_span.cpp`（8 个用例）：validator 对
  "表不存在/列不存在/WHERE 列不存在"给出精确位置、无 resolver 时 span 未知、
  builder 兜底 span、caret 渲染（缩进与 `^` 个数）、`sspan_to_string` 格式、
  `collect_source_spans` 覆盖表名与列名。

结果：`test_parser` 55 → **63 用例**，`test_statement` 29 → **37 用例**，
`test_sql_types` 188 用例；`ctest` 3/3 通过，0 告警。

---

# 2026-09-12 值级定位 + 语法高亮

## 值级定位（错误直接指向字面量）

上一轮只能定位到"名字"（表/列），`INSERT ... VALUES (1, 'abc')` 这种类型错误
指向的是列名 `age`。本轮把 **值自身的 span** 也带上：

| 位置 | 变化 |
|------|------|
| `sql_types/query.h` | `InsertQuery::value_spans`（与 values 对应的二维 span，可为空）+ 安全访问 `value_span(row, col)`；`UpdateQuery::Assignment::value_span` |
| `StatementBuilder` | 填这两个字段（来自 AST 值节点的 `span`）；**不填也不影响任何语义** |
| `StatementValidator::check_value` | 新增 `value_span` 参数，定位优先级：值自身 > 列名（resolver）> 未知 |

效果（实测输出）：

```
INSERT INTO users (id, age) VALUES (1, 'abc');
                                       ^^^^^ Type mismatch @ age (line 1:40-45)
UPDATE users SET age = 'abc' WHERE id = 1;
                      ^^^^^ Type mismatch @ age (line 1:24-29)
```

注意：`value_spans` 是**诊断元数据**，不参与比较/编码/键；
手工构造 Query 时留空即可，validator 会退回用列名定位。

## 语法高亮（ANSI，可选）

新增 `common/ansi_color.h`（颜色常量 + `paint` + `enabled_by_default()`）
与 `statement/sql_highlight.h/.cpp`：

- `highlight_sql(sql, colors)`：关键字粗蓝、类型青、字符串绿、数字黄、`--` 注释暗。
- `highlight_span(sql, span, colors)`：只把出错片段标亮红。
- `sspan_caret(sql, span, label, colors)`：报错行里片段高亮 + caret 同色 +
  信息红色 + 位置暗色；**颜色的开关由调用方决定**，默认 `false`。
- `ansi::enabled_by_default()`：`NO_COLOR` 已设置或 stdout 非终端时为 false，
  CLI 可直接用它当默认值（测试环境下自然不上色）。
- 上色不影响对齐：caret 缩进按原始文本计算，转义只包在片段外层
  （有 `StrippingEscapesRecoversTheOriginalText` 用例保证"去色后 == 原文"）。

## 测试

- `test_source_span.cpp` 37 → 41 用例：INSERT 值 span 指向字面量（含列偏移）、
  同一行多值的 span 各自正确（超长字符串指向第二个值）、UPDATE 值 span、
  没有值 span 时退回列名位置。
- 新增 `tests/test_statement/test_highlight.cpp`（6 用例）：关闭颜色原样返回、
  关键字/类型/字符串/数字上色、去转义后与原文一致、错误片段高亮、
  caret 的高亮与对齐、`NO_COLOR` 策略。

结果：`test_statement` 37 → **47 用例**（290 断言），`test_parser` 63 用例、
`test_sql_types` 188 用例；干净目录 configure+build+ctest **3/3 通过、0 告警**。

---

# 2026-09-12 审核反馈（两条）

## 1. `build()` 改为接收 `const ASTNode*`

反馈：builder 不会修改 AST，接口应是 `const ASTNode*`；
"生命周期交给 builder"是初始设计失误，实际由上层调用者管理。

处理：

- `StatementBuilder::build(const ASTNode*)`，全部 `build_stmt_*` /
  `build_value` / `build_condition` / 各列表转换函数同步改为 `const ASTNode*`。
- 删除了实现里的 **16 处 `const_cast<ASTNode*>`**（`grep -c const_cast` 归零）。
- 头文件注释明确"不接管所有权、不改 AST"；新增用例
  `Builder.AcceptsConstAst`（用 `const ASTNode*` 调用并复用 AST）。

## 2. `StrKeyRange` 的正无穷/边界包含问题

反馈：`{low, high}` 表达不了"超过类型最大值"的场景，
`Value::upper_key_for_type()` 只能给族上界，应该像 `KeyRange` 一样加边界标志。

处理（`sql_types/key_range.h`）：

```cpp
struct StrKeyRange {
  Key low, high;
  bool has_low = true;          // false = -∞
  bool has_high = true;         // false = +∞
  bool low_inclusive = true;
  bool high_inclusive = false;
  bool is_empty() const;                          // [x, x) = ∅
  std::optional<Key> half_open_start() const;     // 无界 -> nullopt
  std::optional<Key> half_open_end() const;       // 包含式 -> +0x00 后继
};
```

- `to_str_key_range()` 现在返回**原始边界 key + 标志**（不再把 `+0x00`
  隐式写进 high）；`half_open_*()` 负责给"只支持 [start,end)"的迭代器做转换，
  无界侧返回 `nullopt`。
- 空集改用 `[x, x)` 表示，不再用"倒置的 low/high"。
- 于是 `id <= BIGINT_MAX`、`id >= INT64_MIN` 这类顶到类型边界的区间可以正确表达
  （族上界不再被误当成上界）。
- 新增用例：`StrKeyRange.UnboundedRangesCarryNoSentinelTrust`、
  `OpenRangeKeepsHalfOpenKeys`、`ClosedRangeCarriesInclusiveFlag`、
  `ClosedRangeAtTypeMaxIsExpressible`、`EmptyRangeIsRepresentedAsHalfOpenEmpty`。

遗留：`storage/range_convert.h`（适配层，当前仍引用已删除的
`has_start()/start()`，本来就不编译）在恢复时应改用
`has_low/has_high/low_inclusive/high_inclusive` 或直接消费 `to_half_open()`。

结果：`test_sql_types` 188 → **193 用例**，`test_statement` 47 → **48 用例**，
`test_parser` 63 用例；`ctest` 3/3 通过、0 告警。

---

## 多行 VALUES + 主键冲突的提前检查

### 1. 语法与 AST：`values` 统一是"行的列表"

`sql.y` 的 `insert_values` 改成左递归的两条规则：

```
insert_values:
    '(' value_item_list ')'                     { $$ = create_list($2, LIST_VALUE); }
    | insert_values ',' '(' value_item_list ')' { $$ = append_to_list($1, $4); }
```

**为什么是左递归 + 统一包一层**：如果保留"单行 = 扁平列表"、另加一条
`row_list` 规则，`')'` 之后用 `;` 还是 `,` 会撞上**reduce/reduce 冲突**
（bison 会按规则顺序硬选一条，语义就成了运气）。统一成"行的列表"之后
没有歧义，单行只是行数为 1，builder 只有一条路径。

代价：`INSERT ... VALUES (1,2)` 的 AST 多一层（`values -> row -> values`），
`test_literals.cpp` 里取"第一个值"的小工具因此改了两行；手工构造扁平的
builder 分支保留（`test_ast_nodes` 那种直接 `make_insert_node` 的用法不受影响）。

### 2. 校验：两类主键冲突都在执行前查出来

`validate_insert()` 末尾调用 `check_primary_key_conflicts()`：

| 冲突 | 怎么查 | 需要什么 |
|------|--------|----------|
| 这一批 VALUES 内部重复 | `std::set<sql::Key>` 去重，key 用 `Value::to_key(主键列类型)` —— **和存储落 key 的方式完全一致** | 无（纯静态） |
| 与表里已有的行冲突 | `catalog_.primary_key_exists(db, table, pk)` | Catalog 支持数据探测；不支持则**降级**为执行期检查 |

- 新增 `sql::Catalog::primary_key_exists(...)`（默认 `nullopt` = 不支持）；
  `KVCatalog` 实现了它（`open_table` + `find`，读的是**本连接**的视图，
  所以事务里能看到自己未提交的写：`BEGIN; INSERT id=1; INSERT id=1;` 第二条
  在校验期就报）。
- 表打不开 / 读到坏行 → `nullopt`（不在这里下结论，交给执行期）—— 校验器
  的职责是"能确定就早点报"，不是替执行器兜底。
- 报错码 `DUPLICATE_PRIMARY_KEY`，位置指向**那个具体的值**（`value_spans`），
  多行时一眼能看出是哪一行。
- session 侧把 `DUPLICATE_PRIMARY_KEY` 统一映射成
  `SessionErrorCode::CONSTRAINT_VIOLATION`：不管是校验期还是执行期发现，
  对外都是"约束冲突"，只是**报得更早、位置更准**。

### 3. 为什么值得（用户的原话：越早发现越好）

写语句在 session 里是"一条语句一个事务"，执行到一半才发现冲突要回滚；
校验期拒掉**事务根本不开**。显式事务里更明显：校验期错误**不中止事务**，
用户可以接着在同一个事务里干活 —— 用例
`MultiInsert.ConflictIsFoundBeforeTheTransactionStarts` 就是钉这个行为。

### 4. 用例

| 位置 | 覆盖 |
|------|------|
| `test_parser/test_sql_statements.cpp`（+2） | 多行 VALUES 解析、AST 形状（2 行 × 2 值、值本身）、逗号写错要报语法错 |
| `test_statement/test_validator.cpp`（+5） | 多行不重复通过；批内重复报 `DUPLICATE_PRIMARY_KEY`；`ProbeCatalog`（假的数据探测）模拟"已有 id=1" → 报冲突、不冲突的 id 通过；Catalog 不支持探测时降级通过；没给主键列/显式 NULL 主键仍报 `COLUMN_ATTR_NULL_MISMATCH` |
| `test_executor/test_write.cpp`（改 1 + 1） | 多行 VALUES 一次插 2 行（`affected_rows == 2`，两行都在）；中途撞主键报错（执行器这层没有事务，语句级原子性由 session 保证） |
| `test_session/test_multi_insert.cpp`（新，5） | 多行插入 + 受影响行数；批内重复 / 与已有行冲突都在执行前报、**一行都不落**、位置指向那一行；显式事务里冲突不中止事务（还能继续干活并提交）；事务里"先插再插同 key"也能在校验期发现 |

结果：`test_parser` 82 → **84 用例**，`test_statement` 53 → **58 用例**，
`test_executor` 43 → **44 用例**，`test_session` 67 → **72 用例**；
`ctest` 11/11 通过、0 告警。
