-  sql_types模块目前是孤岛
  [Fixed] 是的，正在开发重构，先保证该模块的健壮性。

## 一、必须修的硬伤（P0）

  1. key_range.cpp 根本没编进库。CMakeLists.txt:13 只列了 4 个 .cpp，ar t build/libsql_types.a 里没有 key_range.cpp.o。任何调用 KeySet::is_intersect/filter、
     KeyRange::intersects_set/filter_set 的代码都会 undefined symbol（我实测确认）。这两个函数恰好是 KeySet 与 KeyRange 的唯一交互入口。
  [Fixed] 我已经加入了。

  2. KeyRange::subtract 丢失单侧无界的半边。sql_types/key_range.h:389 只在 low_ && other.low_、high_ && other.high_ 同时有界时才产出结果，所以 [1,+∞) - [1,5) 返回 ∅ 而不是 [5,+∞)
     （实测）。测试里 KeyRange.Difference 只覆盖了"双侧有界"这一种情况，正好漏掉它。
  [Todo] 改。

  3. KeySet 的惰性排序标志可以被绕过。sql_types/key_set.h:47 的 points() 和 begin() 非 const 版本直接返回可变引用，不会把 sorted_ 置回 false，而 contains/equals 用的是
     binary_search。实测：集合实际含 {0,1,3} 时 contains(0) 返回 false，且 is_sorted() 仍报 true。这类假阴性在优化器里会静默丢结果。
  [Todo] 改。

  4. RowBuilder::build_ordered 校验错位。sql_types/row.cpp:89 按 order 重排出行后仍调用 schema.validate_row(row)，而 validate_row 是按 schema 位置逐列比较类型。所以只要 order 与
     schema 顺序不同就会报 Type mismatch —— 这正是 RowBuilder.BuildOrdered 失败的原因，而这个函数存在的意义就是允许顺序不同。
  [Todo] 改。

  5. Row::serialize/deserialize 用 '|' 拼接二进制 key。sql_types/row.cpp:22 把编码后的 key 用 | 连接、再按 | 切分，而 key 编码里含原始字符串字节，值里一旦出现 | 就切错。实测：值
     为 "a|b" 时反序列化得到 NULL。同类问题在 sql_types/schema.cpp:166 的 | , : 分隔格式里也存在（带引号/特殊字符的标识符会破坏格式）。建议统一改成长度前缀 framing，并给格式加版
     本号。
  [Todo] 改。

  6. TableSchema::column_at 没有边界检查（sql_types/schema.h:99），column_at(-1)/越界直接是 UB，而测试 Schema.ColumnAccess 期望抛异常。
  [Todo] 改。

  7. RowBuilder 静默丢弃 schema 之外的列。sql_types/row.cpp:66 只遍历 schema 的列去 values_ 里找，从不检查 values_ 中多出来的键。测试期望返回 COLUMN_NOT_FOUND，实际返回成功（实
     测）。这会让拼错列名的 INSERT 静默少写数据。
  [Todo] 改。

  8. 缺 NOT NULL 列时的错误码语义不一致：sql_types/row.cpp:72 直接返回 COLUMN_ATTR_NULL_MISMATCH，测试期望 INVALID_ROW；而且这里用 fprintf(stderr, ...) 在库代码里打日志，测试和嵌
     入式使用都会被打扰。
  [Todo] 改。

## 二、类型与语义层的设计问题（P1）

  - INT/BIGINT 双轨是最值得先定案的问题。 Value(int) 造 INT（key 编码 4 字节）、Value(int64_t) 造 BIGINT（9 字节），但两者 operator== 为真（按 type class 比较），to_key() 却不同
    （实测 key 长度 5 vs 9）——等值语义和索引 key 不一致。
    同时 validate_row 用精确 columns_[i].type != row[i].type()，所以 BIGINT 列配 set(col, 5) 这种 int 字面量会直接 Type
    mismatch（实测 [1] ok=0 err=Type mismatch）。
    要么合并成单一 INT64 逻辑类型，要么在 RowBuilder/校验层做整数拓宽。顺带说一句，sql_types/field_type.h:15 注释写"INT 64位整数"，但
    编码按 32 位。
    [Todo] 改。确实要统一，本来底层存储也是统一int64_t， 但是逻辑类型层怎么处理没想好有点乱。

  - 两套"类型相等"标准并存：Value::operator== 用 get_type_class，schema 校验用精确比较。外加 KeyCodecs::from_key 对 TEXT 列返回 VARCHAR（sql_types/key.h:100），TEXT 值往返一次就会校验失败。
    [Todo] 改。

  - Value 用 union 持指针、手写 5 个特殊成员函数。 拷贝构造里写 int_val_ 再按 bool 读回，标准上是读非活跃 union 成员；bool 与 int64 共用存储。建议改
    std::variant<std::monostate,int64_t,bool,std::string>，或直接放 std::string 成员（SSO 已足够）。另外缺 std::string&&/string_view 构造，Value(std::string) 永远 new + 拷贝。
    [Todo] 改。

  - 类型系统覆盖面窄：没有浮点/定点/日期时间（FLOAT/DOUBLE/DECIMAL/DATE/TIMESTAMP），string_to_data_type 不认 varchar(n)、char、smallint 等，is_numeric() 实际等于 is_integer()。
    [Fixed] 暂时不考虑，简单起见。

  - SQL NULL 三值逻辑没有建模：Value 的 </== 把 NULL 当成"最小且等于自身"，CompareCondition 没有"结果未知"的概念，NOT/OR 的短路语义将来会在这上面踩坑。
    [Todo] 改。

  - 只支持单列主键：TableSchema 只有 primary_key_index_；测试 Schema.CompositePrimaryKey 只是断言"多主键报错"。同时缺 KeyPrefix/复合 key 类型（旧的 relation/key_prefix.h 有，没迁过来）。
  [Fixed] 暂时不考虑多主键，简单起见。keyfix不打算放这个模块。

  - KeyRange 对字符串不完整：closed()、point() 遇到非整数直接返回空集（sql_types/key_range.h:86、sql_types/key_range.h:94），VARCHAR 列的区间/点查询拿不到范围；intersect/
    subtract/complement 的结果会丢类型信息（type_ 取 this 的）；empty() 没有类型，to_str_key_range 只能靠 fallback。
  [Todo] 改。

  - 错误处理四套风格并存：bool（Catalog）、SchemaError 返回码、异常（Value::as_int、KeySet::min/max）、std::expected（RowBuilder），库内还 fprintf(stderr)。建议统一到
    std::expected/错误码，异常只留给真正的编程错误。
  [Todo] 能改则改。

  - 条件树能力偏薄：缺结构相等与哈希（无法去重、合并、缓存）、negate()、CNF/DNF 规范化；语法上缺 BETWEEN、EXISTS、列-列/值-值比较、子查询、LIKE 转义。CompareCondition/InCondition 构造参数用 std::string 而成员是 Identifier，风格不一致。ConditionVisitor 全部是默认空实现，漏写 override 会静默通过，建议至少对关键节点保持纯虚或返回 bool 状态。
  [Todo] ConditionVisitor 同意修改，但是条件树的这些方法不会留在这个模块，会放在优化器中。条件树的构造参数确实需要统一风格。

  - Query 层：Query 因 ConditionPtr 不可拷贝；type_ 与 variant 冗余（有二者不一致的风险）；SelectQuery 无 DISTINCT/JOIN/聚合/HAVING（group_by 自己标注"预留"）；InsertQuery 无DEFAULT/ON CONFLICT。
  [Todo] 不可拷贝，还好吧，有什么问题。其他都是简化处理，暂不改。

  - 头文件靠传递包含：condition_types.h 用 std::find 无 <algorithm>，key_set.h 用 std::runtime_error 无 <stdexcept>，compare_op.h/condition.h 用 uint8_t/size_t 无 <cstdint>/
    <cstddef>，identifier.h 用 std::ostream/tolower 无 <ostream>/<cctype>。当前编译器能过（我逐个 header 单独编译验证过），但换标准库或调换包含顺序就会炸。
   [Todo] 改。都加上。

  - 重复定义与冗余 API：using Key = std::string 在三处重复（value.h/key.h/key_range.h）；identifier.h 同时提供 IdentifierHash 和 std::hash 特化；schema 有 to_string/
    to_string_pretty/to_string_table/to_string_summary 加 ColumnDef::to_string 五套格式化实现，重复度高、维护成本大。
  [Todo] 确实要统一。

  - 与上层未接通：Catalog/DatabaseView 用 std::string 传库表名，与模块内部 Identifier（大小写不敏感）不一致；storage/range_convert.h 引用了新 KeyRange 根本不存在的 has_start()/
    start()。这个模块缺一个把 Query/schema 与 relation/storage 对齐的适配层，否则"新类型"和"旧实现"会持续漂移。
    [Fixed] Catalog/DatabaseView 已改。其他模块暂时不考虑。

## 三、测试与工程（P1/P2）

  当前 5 个失败（./build/run_tests/test_sql_types）：

   用例                                位置                    现象
  ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━  ━━━━━━━━━━━━━━━━━━━━━━  ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
   RowBuilder.MissingRequiredColumn    test_row.cpp:145        期望 INVALID_ROW，实际 COLUMN_ATTR_NULL_MISMATCH
  ──────────────────────────────────  ──────────────────────  ──────────────────────────────────────────────────
   RowBuilder.ColumnNotFound           test_row.cpp:157,158    期望失败，实际构建成功
  ──────────────────────────────────  ──────────────────────  ──────────────────────────────────────────────────
   RowBuilder.BuildOrdered             test_row.cpp:195        期望成功，实际 Type mismatch
  ──────────────────────────────────  ──────────────────────  ──────────────────────────────────────────────────
   Schema.ColumnAccess                 test_schema.cpp:228     期望越界抛异常，未抛

  覆盖面缺口：

  - KeyCodecs 的 to_key ↔ from_key 往返 一个用例都没有，而这正是存储格式的契约，也是上面 INT/BIGINT、TEXT/VARCHAR 问题的所在；应补：各类型往返、边界值（int32/int64 极值）、NULL、
    含 |/空串/超长串、非法 key。
  [Todo] 改。

  - KeyRange::subtract 的无界分支、complement、closed/point 对字符串的行为都没有测试；visitor 的 walk_condition_post_order 没测；LIKE/NULL 语义没测；Catalog/DatabaseView 没测。
  [Todo] 改。

  - tests/test_sql_types/CMakeLists.txt:1 重复设置 include（sql_types 已经是 PUBLIC），且只注册了 ctest，没接进 Makefile 的 test 目标。
  [Todo] 改。

  - 没有 README/契约文档：左闭右开约定、NULL 语义、错误码含义、key 编码格式都只存在于注释和测试里。
  [Todo] 改。

  - KeySet::add() 每次线性查找去重，批量插入是 O(n²)，IN 列表一大会成为热点。
  [Fixed]简单起见，先不改。

  - 仓库里 old/、relation/、statement/ 与 sql_types 三套 Value/Schema/Row 并存（relation/types.h 甚至已不存在），建议明确迁移计划或删除，否则会继续分叉。
   [Fixed]正在重构中，这些模块先不动。

  ## 建议的修复顺序

  先做 P0 的 8 条（其中 1、2、3、5 是正确性/构建问题，4、6、7、8 是实现与测试契约对齐），同时把 KeyCodecs 往返测试补上；再做 P1 里"定契约"的两件事——INT/BIGINT 静一静、序列化格式
  加长度前缀和版本号；然后是错误处理风格统一、条件树能力补全；最后是文档、旧模块清理和 range_convert.h 的重写。

---

# 2026-09-10 修复轮（Codex 执行）

结论：所有 [Todo] 改 的条目已修完（含 P0 全部 8 条），[Fixed] 暂不考虑 的条目未动。
验证：从零 `cmake` 配置 + 编译 + 运行测试通过，`-Wall -Wextra` 零告警，每个头文件单独
编译也通过。测试规模 99 用例/5 失败 → **136 用例 / 794 断言 / 0 失败**。

## 状态更新（替换上面的 [Todo] 标记）

| # | 条目 | 状态 | 修法 |
|---|------|------|------|
| P0-2 | `KeyRange::subtract` 丢单侧无界分支 | [Fixed] | 改为按编码 key 判定，左右两部分各自处理无界；`[1,+∞)-[1,5)=[5,+∞)` |
| P0-3 | `KeySet` 惰性排序标志可被绕过 | [Fixed] | 非 const `points()` 置 `sorted_=false`；删掉非 const `begin()/end()`，只留只读迭代；`rbegin/rend` 补 `ensure_sorted()` |
| P0-4 | `build_ordered` 按 schema 位置校验 | [Fixed] | 改为按列定义 `validate_value(col, value)` 校验 |
| P0-5 | 用 `'|'` 拼二进制 key 的序列化 | [Fixed] | 新增 `sql_types/byte_buffer.h`，`Row`/`TableSchema` 全部改长度前缀 framing + 首字节版本号 |
| P0-6 | `column_at` 无边界检查 | [Fixed] | 越界抛 `std::out_of_range` |
| P0-7 | Builder 静默丢弃 schema 外列 | [Fixed] | `build()` 先检查多余键 → `COLUMN_NOT_FOUND` |
| P0-8 | 缺 NOT NULL 列错误码不一致 + `fprintf` | [Fixed] | 返回 `INVALID_ROW`；库内 `fprintf(stderr)` 全部删除 |
| P1 | INT/BIGINT 双轨 | [Partial] | 编码统一（同族标记 0x01、统一 8 字节保序）+ 新增 `is_same_family()` 按族校验；**枚举是否合并未定**（见下） |
| P1 | 两套"类型相等"标准 | [Fixed] | `validate_value()` 统一用族判定；`from_key` 按调用方要求的逻辑类型构造（TEXT 往返仍是 TEXT） |
| P1 | Value union + 手写 5 个特殊成员 | [Todo] | 本轮未做，与 INT/BIGINT 合并一起做，避免改两遍 |
| P1 | SQL NULL 三值逻辑 | [Todo] | 本轮未做，需先定真值枚举的归属（本模块 or 优化器） |
| P1 | KeyRange 对字符串不完整 / 丢类型 | [Fixed] | 区间判定全部基于编码 key；支持 `(a,b]`/`[a,b]`/`{a}` 与字符串单点/闭区间；集合运算传播类型 |
| P1 | 错误处理风格并存 | [Partial] | 可预期失败统一 `std::expected` + 错误码、编程错误用异常、库内不打日志；`Catalog` 的 bool 与 `Value`/`KeySet` 的异常按"接口已定"保留 |
| P1 | 条件树构造参数风格 + ConditionVisitor | [Fixed] | `CompareCondition`/`InCondition`/`make_*` 统一 `Identifier`；5 个 `visit` 改纯虚，另加 `ConditionVisitorBase`；结构相等/negate/CNF 按约定留给优化器 |
| P1 | 头文件靠传递包含 | [Fixed] | 逐个补 `<algorithm>/<stdexcept>/<cstdint>/<cstddef>/<ostream>/<cctype>/<functional>`，并做了"每个头单独编译"验证 |
| P1 | 重复定义与冗余 API | [Partial] | `using Key` 只保留 `value.h` 一处；schema 的 5 套格式化输出属展示层，本轮未动 |
| P1 | Query 可拷贝性 | [Won't fix] | 保持 move-only；将来需要时用 `shared_ptr<const Condition>` 或拷贝构造里 `clone()` |
| 测试 | KeyCodecs 往返 0 覆盖 | [Fixed] | 新增 `tests/test_sql_types/test_key_codec.cpp`（11 用例） |
| 测试 | KeyRange 边界 / post_order / 序列化 等 | [Fixed] | 新增 `test_serialize.cpp`(8)、`test_catalog.cpp`(3+1)；`KeyRange` 12→22、`Condition` 8→12 |
| 工程 | tests CMake 重复 include、没接 Makefile | [Fixed] | 去掉重复 `target_include_directories`；`Makefile` 新增 `make sql-types` / `make sql-types-test` |
| 工程 | 没有 README/契约文档 | [Fixed] | 新增 `sql_types/README.md`（编码格式、类型族、区间语义、序列化格式、错误处理约定、未决事项） |

测试分布：CompareOp 6 / Value 15 / Identifier 9 / FieldType 6 / Row 6 / RowBuilder 8 /
Condition 12 / KeySet 9 / KeyRange 22 / KeyCodec 11 / Schema 15 / Serialize 8 /
Catalog 3 / DatabaseView 1 / Query 5。

## 本轮新发现（原清单里没有，已修）

**字符串 key 的字节序与值的字典序不一致。**

旧编码是 `[tag][u32 长度][内容]`，长度前缀在内容之前，于是 key 的比较是"长度优先"：
`key("b") < key("aaaa")`，但逻辑上 `"aaaa" < "b"`。而 `KeyRange::contains()` 用值比较、
`to_str_key_range()` 产出的区间用 key 比较 —— 两者会给出不同答案。实测：
`["a","b")` 的 contains 认为 `"aaaa"` 在区间内，但换算到 key 空间后它落在区间外，
即**范围扫描会静默丢数据**。该问题只在把 `to_str_key_range()` 真正接到存储层后才会暴露，
所以之前没被测出来。

修法：
- 字符串编码改为 `0x00 → 0x00 0xFF` 转义 + 结尾 `0x00` 终止符 → 字节序 == 字典序；
- 族上下界从"0xFF 填充的伪上界"改为 `[tag]` / `[tag+1]`（tag 是连续小整数），
  既严格又不会被含 `0xFF` 字节的字符串击穿；
- 新增 `KeyCodecs::inclusive_upper_bound(k) = k + 0x00`，用于表达单点/闭区间的上界；
- 回归测试：`KeyCodec.StringOrderingMatchesValueOrdering`、
  `KeyRange.EncodedRangeMatchesValueSemantics`（断言"contains() 必须等于在编码 key 空间的
  成员判定"，这条在旧编码下会失败）。

## 破坏性变更（上层接入时注意）

1. **Key 编码格式 v2**：tag 不再等于 `DataType` 枚举值（`0x01` int64 / `0x02` string /
   `0x03` bool）；INT 与 BIGINT 同 key；字符串改为保序编码。已有落盘数据需要重建。
2. `Row::deserialize()` / `TableSchema::deserialize()` 返回 `std::expected<..., SchemaError>`；
   序列化格式为 v1（首字节版本号 + 长度前缀）；`Row::deserialize` 现在严格校验字段数与列数
   （不再静默补齐/忽略多余字段）。
3. `SchemaError` 新增 `INVALID_FORMAT`。
4. `Identifier` 的字符串构造去掉 `explicit`（可隐式转换，少写包装）。
5. `ConditionVisitor` 的 5 个 `visit` 变为纯虚；只关心部分节点的 visitor 改继承
   `ConditionVisitorBase`。
6. `CompareCondition`/`InCondition`/`make_compare`/`make_in` 的列参数由 `std::string`
   改为 `Identifier`。
7. `Value(const std::string&, bool)` 技巧移除，改为 `Value(std::string, DataType)`；
   `Value::text()` 现在真的返回 TEXT。
8. `KeyRange` 新增 `low_key()`/`high_key()`/`low_exclusive()`/`high_inclusive()`；
   `empty()` 增加可选类型参数；`to_str_key_range()` 的字符串上下界语义变化。
9. `Value::from_string(INT)` 改用 `stoll`（不再在 32 位边界溢出抛异常）；
   `"NULL"`/`"TRUE"` 等解析改为大小写不敏感。

## 仍未做 / 待决定

- **INT/BIGINT 逻辑类型是否合并**。当前是过渡态：枚举保留区分，编码与校验按族统一，
  行为自洽。推荐方案 A：合并为单一 `INTEGER`（64 位），`BIGINT` 保留为同值解析别名，
  `Value` 只留 `Value(int64_t)`；需要同步改 `string_to_data_type`/`data_type_name`
  及测试期望。方案 B（保留区分 + 隐式提升矩阵）需额外维护转换规则表，不推荐。
  [Todo] 可以合并。加入更多整型，如int8, int16, 并且支持类型提升。
  [Todo] 跨类型比较：支持。SQL 标准不禁止，主流数据库通过“类型提升”实现安全的隐式转换。
  [Todo] 比较时忽略类型吗：不忽略。类型在比较前会被转换为一个能容纳两者的更宽类型。
  [Todo] 存储中类型标识：相同。仅仅Schema中逻辑类型表示不同，底层存储统一，与现在方案一致。在写入和读取时校验范围。
  [Todo]在你的存储引擎设计中，可以借鉴这种“提升到公共类型进行比较”的思路。关键在于确保比较的双方最终能映射到一个具有相同字节序的编码上，这样你的 KeyRange 和索引结构才能正常工作。简单来说，就是Value中统一用int64_t存储，同一个类型family中可以相互比较。逻辑类型仅仅存在于schema和校验中。



- `Value` 的 union → `std::variant<std::monostate,int64_t,bool,std::string>` 重构。
  [Todo] 按此建议修改。

- SQL NULL 三值逻辑（UNKNOWN / IS NULL / AND-OR-NOT 真值表）：目前 `Value` 的 `<`/`==`
  是"存储排序"语义，不是 SQL 比较语义，需要先定接口归属。
  [Todo]  按照MySQL语义，该怎么改？
  [Todo]  存储层没有 NULL 的概念，只有字节。当前NULL在存储中是一个字节'\x0'吗？
  [Todo]  to_key 生成字节串，Key 之间的 < 就是字节序比较。它要保证：
  [Todo]  - 全序（total order）：任意两个 Key 都能比出大小，没有"不可比"。
  [Todo]  - 稳定：同样的值生成同样的 Key。
  [Todo]  - 数值序 == 字节序（通过翻转符号位实现）。
  [Todo]  所以存储层与sql层存在本质的冲突，因为sql层对NULL是三值逻辑。
  [Todo] NULL 三值逻辑：
  [Todo] NULL = 5      -- UNKNOWN
  [Todo] NULL < 5      -- UNKNOWN
  [Todo] NULL = NULL   -- UNKNOWN（注意！不是 TRUE）
  [Todo] NULL <> NULL  -- UNKNOWN
  [Todo]   NULL IS NULL       -- TRUE
  [Todo] NULL IS NOT NULL   -- FALSE
  [Todo] 5 IS NULL          -- FALSE
  [Todo] SQL 里 NULL 不是一个普通值，它表示 UNKNOWN（未知）。任何和 NULL 的比较结果都是 UNKNOWN，不是 TRUE 也不是 FALSE,真值集合是 {TRUE, FALSE, UNKNOWN}.
  [Todo] IS NULL / IS NOT NULL 是专门用来"探测 NULL"的谓词，它们的结果是二值的
  [Todo] WHERE 子句的规则是：只有结果为 TRUE 的行才保留，FALSE 和 UNKNOWN 都过滤掉。
  [Todo] FALSE AND UNKNOWN = FALSE（FALSE 已经"一票否决"）
  [Todo] TRUE OR UNKNOWN = TRUE（TRUE 已经"一票通过"）
  [Todo] 其他和 UNKNOWN 组合都还是 UNKNOWN。
  [Todo] 现在我们约定，Value主要是给逻辑层sql表示用的，所以比较不能直接重载C++的操作符，可能要重定义一套sql_开头专用的三值操作符。而对于原来重载的C++操作符，可能要移动Key中去，因为存储层才需要全序关系，另外要注意null的设计在存储层中如何表示，可能和Schema中的type有关系，Key序列化时要提供输入参数指明所属column的type。

- `storage/range_convert.h` 仍引用已不存在的 `has_start()/start()/has_end()`，
  接上层时按新的 `low_key()/high_key()` 重写。
  [Fixed] 暂时忽略其他模块。
- 按约定暂不考虑：浮点/日期类型、复合主键与 KeyPrefix、`KeySet::add` 的 O(n²)、
  旧模块整合（`old/`、`relation/`、`statement/`）。
  [Fixed] 可以加入日期和时间类型。暂时忽略其他模块。

---

# 2026-09-11 第二轮（整型家族 / variant / 三值逻辑 / 日期时间）

结论：上一轮"仍未做/待决定"的 4 项里，除 collation 与浮点类型外全部落地。
验证：`-Wall -Wextra` 零告警，每个头文件单独编译通过，
测试 136 → **173 用例 / 1043 断言 / 0 失败**。

## 本轮按决策实现的内容

| 决策 | 实现 |
|------|------|
| 整型合并 + 增加 int8/int16 | `DataType` 新增 `TINYINT/SMALLINT`（`INT/BIGINT` 保留），`Value` 一律用 `int64_t` 存储；`Value(5)` 规范化为 `BIGINT` |
| 类型提升 | 新增 `common_type()`：整型取更宽、字符串取 TEXT、DATE+DATETIME 取 DATETIME；跨族只做数值↔字符串、时间↔字符串两条强制转换 |
| 比较时忽略类型？不忽略 | 比较前提升到公共类型（`sql_truth.h` 的 `sql_order`/`sql_compare_op`） |
| 存储中类型标识相同 | key 编码里整型/DATE/TIME/DATETIME 共用一个 family tag（0x01）；`Value::tinyint(5)` 与 `Value::bigint(5)` 的 key 完全相同 |
| 写入/读取校验范围 | `schema.validate_value()` 检查 `can_represent()`（TINYINT/SMALLINT/INT/DATE/TIME/DATETIME），越界返回新增的 `SchemaError::VALUE_OUT_OF_RANGE`；`Row::deserialize` 也会校验 |
| Value 改 variant | `std::variant<std::monostate,int64_t,bool,std::string>`，删掉手写 union/5 个特殊成员函数；补了 `std::string&&`、`string_view` 构造 |
| C++ 操作符移到 Key | `Value` 只保留 `==`/`!=`（容器语义）；全序改为 `KeyCodecs::compare`/`less` 与 `ValueKeyLess`；`KeySet` 的排序/去重/集合运算全部改用 key 比较 |
| NULL 三值逻辑 | 新增 `sql_truth.h`：`Truth{TRUE,FALSE,UNKNOWN}`、真值表、`sql_compare_op`、`sql_equal/sql_less`、`sql_equal_null_safe`、LIKE 匹配、`evaluate_condition`（WHERE 只保留 TRUE） |
| NULL 在存储里的表示 | 旧实现是**空字节串（0 字节）**；现在 key = `[族 tag][null flag][payload]`，NULL 编成 `[族 tag] 0x00`，正好是该族最小 key；逻辑层无列类型时用 `[0x00] 0x00` |
| Key 序列化要指明 column type | 新增 `Value::to_key(DataType column_type)`、`KeyCodecs::to_key(v, type)`、`Row::serialize(schema)` |
| 日期和时间类型 | 新增 `DATE/TIME/DATETIME` + `temporal.h/cpp`（历法换算、严格解析、格式化），`string_to_data_type` 支持 `date/time/datetime/timestamp` |

## 关键语义（新增契约）

- key 格式升到 **v3**：`[family tag][null flag][payload]`；
  族上下界 = `[tag] 0x00`（= 本族 NULL）与 `[tag+1]`，
  所以一次族扫描 `[min, upper)` 恰好覆盖"本族 NULL + 本族全部值"，不串族。
- 族扫描包含 NULL：`min_key_for_type(VARCHAR)` = `[0x02] 0x00`。
- `NULL = NULL` → UNKNOWN；`NULL IS NULL` → TRUE；
  `FALSE AND UNKNOWN` → FALSE；`TRUE OR UNKNOWN` → TRUE；`NOT UNKNOWN` → UNKNOWN；
  WHERE 只保留 TRUE（`NULL <> 5` 的行不会出现在结果里）。
- `x IN (a, NULL)` 未命中时是 UNKNOWN（`NOT IN` 同理，不会误报 TRUE）。
- 跨族比较中"非数字字符串 vs 数值"判 UNKNOWN —— 比 MySQL 的"截断为 0 并
  告警"更保守，避免 `id = 'abc'` 意外命中；这一条如需对齐 MySQL 可再改。
- 时间类型之间不做隐式换算（DATE 天 ≠ DATETIME 秒），写入判类型不匹配。

## 破坏性变更（相对上一轮）

1. **key 格式 v3**：所有 key 多了一个 null flag 字节，族 tag 不再等于
   `DataType` 枚举值，NULL 有了显式编码。落盘数据需重建。
2. `Value` 不再重载 `<` / `>` / `<=` / `>=`；排序请用 `KeyCodecs::compare`
   或 `ValueKeyLess`。`Value::operator==` 仍是容器语义（NULL == NULL 为 true）。
3. `Value(5).type()` 现在是 `BIGINT`（整型规范化），不再是 `INT`；
   依赖 `INT` 的地方（如测试里的 `KeyRange::range(Value(1),...)` 的 `type()`）
   需按 `BIGINT` 判断。
4. `Value` 被移动后是"有效但未指定"状态，不再保证变 NULL。
5. `Row::deserialize` 会额外做范围校验（新增 `VALUE_OUT_OF_RANGE`）。
6. `CDataType` 追加了 `DT_TINYINT/DT_SMALLINT/DT_DATE/DT_TIME/DT_DATETIME`
   （追加在末尾，原有枚举值不变）。

## 仍未做

- collation（字符串比较与 LIKE 目前是大小写敏感的二进制比较）。
- 浮点/定点类型（FLOAT/DOUBLE/DECIMAL）、DATETIME 时区、TIMESTAMP 自动更新。
- 复合主键与 KeyPrefix、`KeySet::add` 的 O(n²)、旧模块整合。
- `storage/range_convert.h` 的适配（等其他模块接入时一起改）。

---

# 2026-09-11 第三轮（字符串 family 的声明长度）

问题：字符串这一族没有按整型 family 的同一套约定处理 ——
检查结论是 **不满足**，本轮补齐。验证：`-Wall -Wextra` 零告警，
测试 173 → **188 用例 / 1159 断言 / 0 失败**。

## 检查到的缺口（修改前）

| 检查项 | 修改前 | 现状 |
|--------|--------|------|
| `VARCHAR(n)` / `CHAR(n)` DDL 解析 | `string_to_data_type("varchar(10)")` 返回 `UNKNOWN` | 新增 `parse_type_with_length()`，`CHAR/VARCHAR` 支持 `(n)`，`TEXT` 不接受长度 |
| 声明长度的存放位置 | 无处可放（`ColumnDef` 只有 name/type/nullable/primary_key） | `ColumnDef::length`（0 = 未声明），与"逻辑宽度只在 schema"一致 |
| 写入范围校验 | 只校验整型/时间，**字符串完全不校验**（10 万字节也能写进 VARCHAR） | `can_represent_length()` 按字节校验，超长返回 `VALUE_OUT_OF_RANGE` |
| 读取范围校验 | 同上 | `Row::deserialize()` 走同一 `validate_value()`，超长旧数据同样被拒 |
| 声明长度是否进 key | — | **不进**：CHAR/VARCHAR/TEXT 共用族 tag `0x02`，同一值 key 相同、NULL key 相同（与整型 `TINYINT..BIGINT` 共用 `0x01` 同构） |
| 类型提升 | 只有 `VARCHAR->TEXT` | 明确 `CHAR -> VARCHAR -> TEXT`；容量用 `common_string_capacity()` 取较大者 |
| 非法声明 | 无从判定 | `CHAR(300)` / `VARCHAR(0)` / `VARCHAR(65536)` / `TEXT(10)` / `INT(4)` → `INVALID_COLUMN_DEF` |
| CHAR 类型 | 不存在 | 新增 `DataType::CHAR`（含 `DT_CHAR`），容量上限 255 字节 |

## 语义约定（与整型对齐）

- 整型：宽度是**值的表示**（`Value(5, TINYINT)`），存储统一 int64；
  字符串：容量是**列的约束**（值本身没有"声明长度"），存储就是字符串本身。
  两者共同点：逻辑类型的信息**只在 schema/校验**，不参与 key 编码。
- 长度单位是字节：`VARCHAR(5)` 放不下 `"中文"`（6 字节），`VARCHAR(6)` 可以。
- 超长一律报错，不做静默截断（对齐 MySQL 严格模式）。
- 未声明长度的 `VARCHAR` 默认容量 65535（与 TEXT 相同），避免历史行为突变。
- `ColumnDef::operator==` 现在包含 length，所以 `VARCHAR(10)` 与 `VARCHAR(20)`
  是两个不同的列定义；schema 序列化升到 **v2**（每列多一个 u32 长度字段）。

## 已知差异（本轮明确不做）

- CHAR 不做空格填充（MySQL PAD SPACE 会让 `'abc' = 'abc '`，我们判不等）。
- 比较按完整字节序，绝不因声明长度截断。
- 没有 MEDIUMTEXT/LONGTEXT/BLOB/VARBINARY、没有字符集与 collation
  （长度按字节，不按字符）。

---

# 2026-09-12 StrKeyRange 定稿 + KeyRange 的 NULL 语义

讨论结论（两轮评审后定稿）：**边界用具体 key，开闭用标志位，NULL 是一等边界。**

## 1. StrKeyRange（物理扫描形式）

```cpp
struct StrKeyRange {
  Key low, high;                 // -∞ = [tag][0x00]（NULL 的 key）；+∞ = [tag+1]
  bool low_inclusive = true;     // 左闭
  bool high_inclusive = false;   // 右开
  bool is_empty() const;         // [x,x) 与 low>high 为空；[x,x] 是单点
  Key half_open_start() const;   // 只支持 [start,end) 的迭代器用
  Key half_open_end() const;
};
```

- **去掉了上一轮的 `has_low`/`has_high`**：无界用具体 sentinel 表达，
  消费方不需要额外状态；两个 sentinel 都是文档化常量且不与真实值的 key 冲突
  （族最小值就是 NULL 的 key；族上界 `[tag+1]` 不被任何值产生）。
- **开闭不靠"边界 ±1"**：`<= 类型最大值`、`> x`、`(a, b]` 都能直接表达，
  不会有整数溢出，也不要求调用方理解字符串编码的后继约定；
  `+0x00` 的后继转换只留在 `half_open_start()/half_open_end()`。
- 空集用 `[x, x)` 表示（不再用倒置的 low/high）。

## 2. KeyRange 的 NULL 能力（优化器直接可用）

| 谓词 | 工厂 | NULL 语义 |
|------|------|-----------|
| 无谓词全表 | `all(type)` | 含 NULL |
| `IS NULL` | `null_only(type)` | 只有 NULL（单点） |
| `IS NOT NULL` | `non_null(type)` | 不含 NULL |
| `> v` / `>= v` / `< v` / `<= v` / `= v` | `gt/ge/lt/le/eq` | **都不含 NULL**（三值逻辑：NULL 不满足任何比较） |
| 参数为 NULL | 同上 | 空集（比较永不成立）；`eq(NULL,type)` = `IS NULL` |

- 物理表示：含 NULL = `low = [tag][0x00]` 含；不含 NULL = `low = [tag][0x00]` 排他；
  只有 NULL = `[NULL, NULL]`。
- 有具体值下界的区间天然不含 NULL。
- 查询：`null_scope()`（`INCLUDE`/`EXCLUDE`/`ONLY`）、`includes_null()`、
  `contains(NULL)`；集合运算自动保持语义（都基于 key 比较）。
- 新增 `KeyCodecs::first_value_key(type)` = `[tag][0x01]`：
  所有非 NULL 值 key 的前缀，是"排除 NULL"的等价写法（与 `[tag][0x00]` 排他等价，
  两者之间不存在合法 key）。
- 实现细节：NULL 排他的下界用"后继 key"表示（而不是 `first_value_key`），
  这样 `complement(non_null)` 与 `non_null` 在 key 空间里**首尾相接**，
  `is_adjacent()/unite()` 才能把它们合并回 `all()`。

## 3. tag / sentinel 约定

- family tag ∈ {0x00(NULL), 0x01(int64+时间), 0x02(string), 0x03(bool)}，永远 < 0x0F；
  **没有任何值的 key 以 0xFF 开头**，因此 `[0xFF]` 可安全用作"全局 +∞"
  （也是 `upper_key_for_type(UNKNOWN)` 的现状），还可用于"前缀内所有键"的上界。
- 该"保留 0xFF 段"要作为硬约束写进文档，并有测试守着。

## 4. 测试

新增/重写 `test_key_range.cpp` 中的 `StrKeyRange` 与 `KeyRangeNull` 两组用例
（共 12 个）：具体 sentinel、开闭标志、半开转换、`<= INT64_MAX`、
空集表示、`all/non_null/null_only` 的 NULL 语义、`contains(NULL)`、
集合运算保持 NULL 语义、比较谓词工厂（含"参数为 NULL → 空集"）、
`(a,b]` 开闭往返、字符串/布尔/时间族排除 NULL 后仍覆盖全部值。

结果：`test_sql_types` 188 → **201 用例**（1299 断言），
`test_parser` 63、`test_statement` 48 用例；干净目录 configure+build+ctest
3/3 通过、0 告警。

## 5. 遗留

- `storage/range_convert.h` 仍未适配（引用已删除的 `has_start()/start()`）；
  恢复时直接用 `StrKeyRange` 的 `low/high + low_inclusive/high_inclusive`，
  或交给 `half_open_start()/half_open_end()`。
- 优化器侧还没接：将来在谓词→范围推导时按上表选择
  `all/non_null/null_only/gt/ge/lt/le/eq` 即可，不需要再手工拼 key。

---

# 2026-09-12 回归 KISS：StrKeyRange 压平为半开区间 + KeyRange 完备性审计

反思结论：上一轮把开闭标志传给物理层，**违背了"扫描层只认一种形式"的初衷**——
复杂度被推给了每个消费方（漏看 `high_inclusive` 就会静默少扫一条数据）。
而且当初"必须加 flag"的技术理由不成立：在我们的编码里 `key + 0x00` 是
**普适后继**（不溢出、不越族、不会被解码成合法值、不与任何值的 key 冲突），
所以四种开闭都能压平进 key。

## 改动

1. **`StrKeyRange` 收敛为 `{Key start, end; bool is_empty();}`**（半开区间）。
   `to_str_key_range()` 成为唯一的"边界压平"点：
   无界 → 族最小值 / 族上界；开下界 → `key+0x00`；闭上界 → `key+0x00`；空集 → `start == end`。
   新增 `KeyCodecs::first_value_key()`（`[tag][0x01]`）作为"排除 NULL"的等价写法说明。
2. **flags 留在 KeyRange 内部**（`low_exclusive_` / `high_inclusive_`），
   优化器/日志要看语义时读它们；物理层不再需要任何标志。
3. **三条不变量测试**：
   - `SuccessorIsUniversalAndSafe`：k < k+0x00、`is_valid_key(k+0x00) == false`、
     不越族（首字节不变且 < 族上界）、同族样本中没有合法 key 落在两者之间；
   - `FlattenMatchesContainsSemantics`：10 个典型区间 × 7 个探针（含 NULL），
     断言 `contains(v)` 与"扫描 [start,end)"完全一致；
   - `EmptyAndPointFlattening`：空集 `start >= end`；单点 `start = key(v)`、
     `end = key(v)+0x00`（与 `point()` 一致）。

## 完备性审计发现并修掉的问题

| # | 问题 | 处理 |
|---|------|------|
| 1 | `range(a, NULL)` / `to(NULL)` 把 NULL 当"无界"，语义与新模型冲突 | NULL 现在是具体边界；无值比 NULL 更小 → 这两种写成**空集**；"无上界"请用 `from(v)`/`all()` |
| 2 | `KeyRange::point(v)` 对整数用 `[v, v+1)` → `v == INT64_MAX` 时**有符号溢出** | 统一改成 `[v, v]`（闭区间），压平后 `end = key(v)+0x00` |
| 3 | `KeySet::to_range()/to_ranges()` 同样做 `back()+1` | 改用 `KeyRange::closed(...)`，溢出消失 |
| 4 | `(-∞, NULL)` 这种"没有值比 NULL 更小"的区间被当成非空 | `is_empty()` 增加该退化情形；`complement(null_only)` 不再多出空扫的区间 |
| 5 | 同一区间可能有两种结构（`low=NULL 含` vs `low=nullopt`），导致 `unite(null_only, non_null)` 不等于 `all()`、`equals()` 判不等 | 新增 `normalize()`：把"含 NULL 的下界"折叠成"无下界"；工厂与集合运算结果统一调用 |
| 6 | 类文档被 clang-format 折行折坏；`to_string()` 对"只有 NULL"打印成 `(-∞, NULL]` | 文档重写；`null_scope()==ONLY` 时打印 `{NULL}` |
| 7 | `point_value()` 在下界为 nullopt 时可能解引用空 optional | 已加保护（`(-∞, NULL]` 返回 NULL） |

集合运算关系现在是显式可验证的（都有测试）：
`all ∩ non_null = non_null`、`all − non_null = null_only`、
`null_only ∪ non_null = all`、`complement(null_only) = non_null`、
`non_null − null_only = non_null`。

## 结果

`test_sql_types` 201 → **209 用例**（1497 断言），`test_parser` 63、
`test_statement` 48 用例；干净目录 configure+build+ctest 3/3 通过、0 告警。

## 仍未做

- `storage/range_convert.h` 仍未适配（引用已删除的 `has_start()/start()`）：
  现在只需读 `StrKeyRange::start/end` 即可（半开区间直接喂给 LevelDB 迭代器）。
- 优化器尚未接谓词→范围：按 README 的工厂表选择即可。
