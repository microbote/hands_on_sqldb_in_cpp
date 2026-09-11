# sql_types —— SQL 领域类型模块

本模块只负责 **SQL 领域的值与类型**（类型、值、行、表结构、条件树、key 区间、
key 编码、序列化、三值逻辑、时间类型）。它不依赖存储引擎、不包含
Executor / Optimizer / Planner，是上层模块（relation / statement / storage
适配层）的公共底座。

## 文件一览

| 文件 | 说明 |
|------|------|
| `field_type.h` | `DataType` / `DataTypeClass`、类型族、类型提升、整型/字符串取值范围、`VARCHAR(n)` 解析、名称互转、C-API 互转 |
| `value.h` / `value.cpp` | `Value`（variant 存储）、相等、文本/时间解析、key 编解码入口 |
| `key.h` | `KeyCodecs`：Value ↔ key 编码（存储格式契约）、全序比较 |
| `key_range.h` / `key_range.cpp` | `KeyRange`：key 空间区间与集合运算 |
| `key_set.h` | `KeySet`：点集与集合运算 |
| `row.h` / `row.cpp` | `Row`、`RowBuilder`、行序列化 |
| `schema.h` / `schema.cpp` | `ColumnDef`、`TableSchema`、`SchemaError`、schema 序列化与校验 |
| `condition.h` / `condition_types.h` / `condition_visitor.h` | 条件树与访问者 |
| `compare_op.h` / `compare_op.cpp` | `CompareOp` 及其特性判断 |
| `sql_truth.h` | **SQL 三值逻辑**：`Truth`、真值表、`sql_compare_op`、类型提升比较、LIKE、WHERE 求值 |
| `temporal.h` / `temporal.cpp` | DATE / TIME / DATETIME 的解析、格式化与历法换算 |
| `query.h` / `query_clause.h` | `Query` 变体、SELECT/INSERT/UPDATE/DELETE/DDL |
| `catalog.h` / `db_view.h` | 纯逻辑元数据接口（Catalog / DatabaseView） |
| `identifier.h` | `Identifier`：保留原始写法 + 大小写无关比较/哈希 |
| `byte_buffer.h` | 长度前缀的字节缓冲读写（序列化用） |

## 核心不变量

这些不变量由 `tests/test_sql_types` 覆盖，改代码时请保持。

### 1. 类型系统与类型提升（`field_type.h`）

- 逻辑类型：`TINYINT / SMALLINT / INT / BIGINT`（整型）、`VARCHAR / TEXT`、
  `BOOLEAN`、`DATE / TIME / DATETIME`、`NULL`、`UNKNOWN`。
- **整型只在 schema/校验层有宽度区别**：`Value` 一律用 `int64_t` 存储，
  编码完全相同；写入/读取时用 `can_represent()` 做范围校验（300 写进
  `TINYINT` 列 → `VALUE_OUT_OF_RANGE`）。
- 类型族：整型一簇、字符串一簇、时间一簇。`is_same_family()` 决定
  "值能否写进该列"；`common_type()` 给出比较时提升到的公共类型
  （`TINYINT+INT -> INT`，`DATE+DATETIME -> DATETIME`，
  `VARCHAR+TEXT -> TEXT`）。
- 时间类型之间不隐式换算：`DATE`（天）与 `DATETIME`（秒）单位不同，
  写入时判 `COLUMN_TYPE_MISMATCH`，比较时按秒提升。

### 1b. 字符串 family 的容量（`field_type.h` / `ColumnDef::length`）

与整型"逻辑宽度只在 schema/校验层"完全同构：

| 类型 | 声明长度 | 容量（字节） |
|------|----------|--------------|
| `CHAR(n)` | 1 ~ 255 | n；未声明时 255 |
| `VARCHAR(n)` | 1 ~ 65535 | n；未声明时 65535 |
| `TEXT` | 不接受声明长度 | 65535 |

- 声明长度存在 **`ColumnDef::length`**（0 = 未声明），只参与 schema 校验；
  `Value` 不存长度，**key 编码里也没有长度**——所以 `CHAR/VARCHAR/TEXT`
  共用字符串族 tag `0x02`，同一个值的 key 完全相同，NULL key 也相同
  （与整型 `TINYINT..BIGINT` 共用 `0x01` 是同一套约定）。
- 写入校验：`can_represent_length()` 按**字节数**与列容量比较，
  超长返回 `VALUE_OUT_OF_RANGE`（等价 MySQL 严格模式，不做静默截断）。
  长度单位是字节，UTF-8 中文一个字 3 字节。
- 读取校验：`Row::deserialize()` 走同一条 `validate_value()`，
  所以「schema 收窄 / 数据被改写」导致的超长旧数据也会被拒绝。
- DDL 层：`string_to_data_type()` 只认类型名，带长度的用
  `parse_type_with_length("VARCHAR(32)", type, length)`；非法声明
  （`CHAR(300)`、`VARCHAR(0)`、`TEXT(10)`、`INT(4)`）在
  `TableSchema::add_column()` 阶段就报 `INVALID_COLUMN_DEF`。
- 提升规则：`CHAR -> VARCHAR -> TEXT`（`common_type`）；需要推导"结果列
  多长"时用 `common_string_capacity()` 取较大容量（`CONCAT` 的"求和"规则
  不在这里，由 SQL 层决定）。
- **不做的两件事**（已知差异）：CHAR 不做空格填充（MySQL 的 PAD SPACE
  collation 会让 `'abc' = 'abc '`，我们是二进制比较判不等）；比较按完整
  字节序，永远不会因为声明长度而截断。

### 2. Value（`value.h`）

- 存储：`std::variant<std::monostate, int64_t, bool, std::string>`。
  整数/时间都是 `int64_t`，字符串用 `std::string`（SSO），NULL 用 `monostate`。
- `Value(5)` 的逻辑类型规范化为 `BIGINT`（最宽的整型）；需要保留宽度时
  用 `Value(5, DataType::TINYINT)` / `Value::tinyint(5)`。
- **只保留 `operator==`/`!=`（容器语义，NULL 与 NULL 视为相等）**，
  `<` / `>` 等排序操作符已移出：
  - 存储层全序 → `KeyCodecs::compare()` / `KeyCodecs::less()`（NULL 最小）；
  - SQL 比较语义 → `sql_truth.h` 的 `sql_compare_op()`（NULL 参与 → UNKNOWN）。
- 被移动对象是"有效但未指定"状态（标准行为），不再保证变成 NULL。

### 3. Key 编码格式 v3（`key.h`）

```
key = [1 字节 family tag][1 字节 null flag][payload]
  null flag: 0x00 = NULL, 0x01 = 有值
```

| tag | 类型族 | payload |
|-----|--------|---------|
| `0x01` | 整型 + DATE/TIME/DATETIME | `(uint64)v ^ (1<<63)`，大端 8 字节 |
| `0x02` | CHAR / VARCHAR / TEXT | 转义后的字节 + `0x00`（`0x00` → `0x00 0xFF`） |
| `0x03` | BOOLEAN | `0x00` / `0x01` |
| `0x00` | 无列类型信息的 NULL | 无（只有 flag 字节） |

- 字符串用转义 + 终止符编码，保证 **key 字节序 == 字典序**（早期"长度前缀"
  会让 `"b" < "aaaa"`，范围扫描会丢数据）。
- **NULL 的编码依赖列类型**：
  - 存储层用 `Value::to_key(column_type)`（或 `KeyCodecs::to_key(v, type)`），
    NULL 编成 `[本族 tag] 0x00`，正好是该族的最小 key；
  - 逻辑层（KeySet 排序等）没有列类型时用 `Value::to_key()`，
    NULL 编成 `[0x00] 0x00`，排序在所有值之前。
- 族的上下界：`min_key_for_type(type)` = `[tag] 0x00`（含 NULL），
  `upper_key_for_type(type)` = `[tag+1]`。因此一次族扫描
  `[min, upper)` 恰好覆盖"本族 NULL + 本族所有值"，不会串到别的族。
- `inclusive_upper_bound(k) = k + 0x00`：用于表达单点/闭区间上界。
- 解码失败返回 NULL（不抛异常）；解码时保留调用方要求的逻辑类型
  （DATE 仍是 DATE，TEXT 仍是 TEXT）。

### 4. SQL 三值逻辑（`sql_truth.h`）

`Truth ∈ {TRUE, FALSE, UNKNOWN}`，NULL 表示 UNKNOWN：

| 表达式 | 结果 |
|--------|------|
| `NULL = 5` / `NULL < 5` | UNKNOWN |
| `NULL = NULL` / `NULL <> NULL` | UNKNOWN（不是 TRUE！） |
| `NULL IS NULL` | TRUE |
| `5 IS NULL` | FALSE |
| `FALSE AND UNKNOWN` | FALSE |
| `TRUE OR UNKNOWN` | TRUE |
| `NOT UNKNOWN` | UNKNOWN |

- `WHERE` 只保留结果为 TRUE 的行：FALSE 与 UNKNOWN 都被过滤
  （`where_keeps()`），`NULL <> 5` 的行不会出现在结果里。
- `x IN (a, b)` ≡ `x = a OR x = b`；列表里含 NULL 且未命中时结果是
  UNKNOWN（`NOT IN` 同理，不会意外返回 TRUE）。
- `sql_equal_null_safe()` 用于 DISTINCT / GROUP BY / JOIN 键：NULL 与 NULL
  视为同一组。
- 跨族比较只做两条 MySQL 风格的强制转换：数值 vs 字符串（字符串按整数
  解析）、时间 vs 字符串（按该时间类型解析）。**解析失败判 UNKNOWN**，
  这比 MySQL 的"截断为 0 并告警"更保守（避免 `id = 'abc'` 意外命中）。
- 条件树求值：`evaluate_condition(cond, lookup)`（`lookup` 返回 nullptr
  表示列不存在 → UNKNOWN）。
- LIKE：`%` 任意长度、`_` 单字符、`\` 转义。**目前是大小写敏感的二进制
  比较**，collation 尚未实现。

### 5. 时间类型（`temporal.h`）

| 类型 | 存储 | 文本格式 |
|------|------|----------|
| DATE | 距 1970-01-01 的天数 | `YYYY-MM-DD` |
| TIME | 自 00:00:00 起的秒数 | `HH:MM[:SS]` |
| DATETIME | Unix 秒（UTC，无时区） | `YYYY-MM-DD[ T]HH:MM[:SS]` |

取值范围：DATE 覆盖 0001-01-01 ~ 9999-12-31；TIME 为 [0, 86399]。
解析严格校验（闰年、月/日/时/分/秒边界），失败返回 `nullopt`。

### 6. KeyRange 语义（`key_range.h`）

- 默认 `[low, high)`；`low_ == nullopt` 是 -∞，`high_ == nullopt` 是 +∞；
  NULL 边界视为无界。
- 两个方向标记：`low_exclusive_`、`high_inclusive_`，可表达
  `(a, b]`、`[a, b]`、`{a}`。
- 所有判定（contains / overlaps / covers / intersect / unite / subtract /
  complement）都基于编码后的 key 字节序，保证 `contains()` 的答案与
  "扫描 `to_str_key_range()` 得到的区间"完全一致；字符串单点/闭区间因此可用。
- 集合运算会传播已知类型（`type()`）。

### 7. 序列化格式 v1（`row.cpp` / `schema.cpp`）

两者都用 `byte_buffer.h` 的**长度前缀 framing**，不用 `|` `,` `:` 分隔符
（key 编码是二进制、标识符可能包含这些字符，用分隔符必然被内容击穿）。

```
Row:    [u8 version][u32 field_count]{ [str key] }*
Schema: [u8 version][str table_name][u32 pk_slot][u32 column_count]
        { [str name][u8 type][u8 flags] }*      # pk_slot 0 = 无主键
```

- `Row::serialize(schema)` 用列类型编码每个字段（推荐）；
  `Row::serialize()` 无 schema，NULL 用无类型 NULL key。
- 反序列化返回 `std::expected`：`INVALID_FORMAT`（损坏/版本不符）、
  `COLUMN_SIZE_MISMATCH`（字段数与 schema 不符）、`VALUE_OUT_OF_RANGE`
  （落盘值与列宽不符）。

### 8. 错误处理约定

- 可预期的失败（解析、校验、反序列化、构建行）→ `std::expected` + 错误码。
- 编程错误（类型用错、下标越界）→ 异常：`Value::as_int()` 抛
  `std::runtime_error`，`TableSchema::column_at()` 抛 `std::out_of_range`，
  文本解析失败抛 `std::invalid_argument` / `std::out_of_range`。
- **库代码不打印日志**，由调用方决定怎么报错。

## 构建与测试

```bash
cmake --build build -j4                 # 编译 sql_types 与测试
./build/run_tests/test_sql_types        # 运行测试
make sql-types-test                     # 等价的一步命令
```

## 仍未做 / 边界

- 字符串比较与 LIKE 使用二进制 collation（大小写敏感），未实现
  `utf8mb4_*_ci` 之类的排序规则。
- 没有浮点/定点（FLOAT/DOUBLE/DECIMAL）；DATETIME 无时区，也没有
  `TIMESTAMP` 的自动更新语义。
- 字符串只有 CHAR / VARCHAR / TEXT：没有 MEDIUMTEXT / LONGTEXT / BLOB /
  VARBINARY，也没有字符集与 collation（长度按字节算，不是按字符算）。
- CHAR 不做空格填充语义（按定长容量校验，但存储不补空格）。
- 时间类型之间不隐式换算（写入时 DATE ≠ DATETIME），需要 SQL 层显式转换。
- 跨族比较中"非数字字符串 vs 数值"判 UNKNOWN（MySQL 会截断为 0 并告警）。
- 只支持单列主键；复合主键与 `KeyPrefix` 不在本模块。
- `KeySet::add()` 用线性查找去重，批量插入是 O(n²)。
