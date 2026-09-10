# sql_types —— SQL 领域类型模块

本模块只负责 **SQL 领域的值与类型**（类型、值、行、表结构、条件树、key 区间、
key 编码、序列化）。它不依赖存储引擎、不包含 Executor / Optimizer / Planner，
是上层模块（relation / statement / storage 适配层）的公共底座。

## 文件一览

| 文件 | 说明 |
|------|------|
| `field_type.h` | `DataType` / `DataTypeClass`、类型族判定、名称互转、C-API 互转 |
| `value.h` / `value.cpp` | `Value`（标量 + 字符串 + NULL）、比较、`to_key`/`from_key` |
| `key.h` | `KeyCodecs`：Value ↔ key 编码（存储格式契约） |
| `key_range.h` / `key_range.cpp` | `KeyRange`：key 空间区间与集合运算 |
| `key_set.h` | `KeySet`：点集与集合运算 |
| `row.h` / `row.cpp` | `Row`、`RowBuilder`、行序列化 |
| `schema.h` / `schema.cpp` | `ColumnDef`、`TableSchema`、`SchemaError`、schema 序列化 |
| `condition.h` / `condition_types.h` / `condition_visitor.h` | 条件树与访问者 |
| `compare_op.h` / `compare_op.cpp` | `CompareOp` 及其特性判断 |
| `query.h` / `query_clause.h` | `Query` 变体、SELECT/INSERT/UPDATE/DELETE/DDL |
| `catalog.h` / `db_view.h` | 纯逻辑元数据接口（Catalog / DatabaseView） |
| `identifier.h` | `Identifier`：保留原始写法 + 大小写无关比较/哈希 |
| `byte_buffer.h` | 长度前缀的字节缓冲读写（序列化用） |

## 核心不变量

这些不变量由 `tests/test_sql_types` 覆盖，改代码时请保持。

### 1. Key 编码格式 v2（`key.h`）

```
key = [1 字节 tag][payload]
```

| tag | 类型族 | payload |
|-----|--------|---------|
| `0x01` | INT / BIGINT | `(uint64)v ^ (1<<63)`，大端 8 字节 |
| `0x02` | VARCHAR / TEXT | 转义后的字节 + `0x00` 终止符 |
| `0x03` | BOOLEAN | `0x00` / `0x01` |

- **tag 是"族"标记，不等于 `DataType` 的枚举值**。枚举重排不会影响落盘格式。
- 字符串的转义规则：`0x00` → `0x00 0xFF`，结尾补 `0x00` 终止符。
  这样 **key 的字节序 == 字符串的字典序**；早期的"4 字节长度前缀"会让
  `"b" < "aaaa"`（长度优先），使范围扫描扫不到数据。
- `NULL` 没有 key（空串），不参与索引。
- 同族上下界就是 `[tag]` 与 `[tag+1]`：`min_key_for_type` / `upper_key_for_type`。
- `INT` 与 `BIGINT` 编码完全相同（与 `Value::operator==` 的族比较保持一致）。
- `inclusive_upper_bound(k) = k + 0x00`：配合终止符编码，它正好是"紧跟 k 之后"
  的 key，用于表达单点/闭区间的上界。

### 2. 类型族（`field_type.h`）

`INT/BIGINT` 底层都是 `int64_t`，`VARCHAR/TEXT` 底层都是字符串。因此
"列类型 vs 值类型"的一致性用 `is_same_family()` 判定，而不是枚举精确相等：
`Value(int64_t(5))` 可以写入 `BIGINT` 列，TEXT 值往返后也不会被误判成 VARCHAR。

不做精度收窄检查（`BIGINT -> INT` 也视为兼容），是否检查取值范围由上层决定。

### 3. KeyRange 语义（`key_range.h`）

- 默认 `[low, high)`；`low_ == nullopt` 是 -∞，`high_ == nullopt` 是 +∞。
- 两个方向标记：`low_exclusive_`、`high_inclusive_`，因此
  `(a, b]`、`[a, b]`、`{a}` 都能精确表达。
- **所有判定（contains / overlaps / covers / intersect / unite / subtract /
  complement）都基于编码后的 key 字节序**，保证 `contains()` 的答案与
  "扫描 `to_str_key_range()` 得到的区间"完全一致。
- 字符串单点/闭区间因此可用：`KeyRange::point(Value(std::string("a")))`。
- 集合运算会传播已知类型（`type()`），不再退化成 `UNKNOWN_TYPE`。

### 4. 序列化格式 v1（`row.cpp` / `schema.cpp`）

两者都用 `byte_buffer.h` 的**长度前缀 framing**，不用 `|` `,` `:` 分隔符
（key 编码是二进制、标识符可能包含这些字符，用分隔符必然被内容击穿）。

```
Row:    [u8 version][u32 field_count]{ [str key] }*     # 长度 0 = NULL
Schema: [u8 version][str table_name][u32 pk_slot][u32 column_count]
        { [str name][u8 type][u8 flags] }*              # pk_slot 0 = 无主键
```

- 版本号在首字节，反序列化会校验；格式变化必须递增常量。
- 失败返回 `std::expected<..., SchemaError>`：`INVALID_FORMAT`（损坏/版本不符）、
  `COLUMN_SIZE_MISMATCH`（字段数与 schema 不符）。

### 5. 错误处理约定

- 可预期的失败（解析、校验、反序列化、构建行）→ `std::expected` + 错误码。
- 编程错误（类型用错、下标越界）→ 异常：`Value::as_int()` 抛
  `std::runtime_error`，`TableSchema::column_at()` 抛 `std::out_of_range`。
- **库代码不打印日志**（不 `fprintf(stderr, ...)`），由调用方决定怎么报错。

### 6. RowBuilder

- `build()`：schema 之外的列 → `COLUMN_NOT_FOUND`；缺 NOT NULL 列 → `INVALID_ROW`。
- `build_ordered()`：按给定顺序产出，校验时按 **列定义** 而不是 schema 位置
  （该 API 存在的意义就是允许行顺序与 schema 不同）。

## 构建与测试

```bash
cmake --build build -j4                 # 编译 sql_types 与测试
./build/run_tests/test_sql_types        # 运行测试
make sql-types-test                     # 等价的一步命令
```

## 已知边界 / 未决事项

- **逻辑类型是否合并 INT/BIGINT** 尚未定案。当前做法是"枚举保留区分、
  编码与校验按族统一"，因此行为是自洽的；若要彻底合并，只需改
  `field_type.h` 的枚举与 `string_to_data_type`。
- `Value` 内部仍用手写 union + `new std::string`（见 `value.h`），
  计划改为 `std::variant<std::monostate, int64_t, bool, std::string>`，
  顺带去掉手写的 5 个特殊成员函数。
- SQL NULL 的三值逻辑（UNKNOWN / IS NULL / AND-OR-NOT 真值表）尚未建模，
  目前 `Value` 的 `<`/`==` 是**存储排序**语义（NULL 最小），不是 SQL 比较语义。
- 只支持单列主键；复合主键与 `KeyPrefix` 不在本模块（`TableSchema` 只有
  `primary_key_index_`）。
- `KeySet::add()` 用线性查找去重，批量插入是 O(n²)，大 IN 列表需要优化。
