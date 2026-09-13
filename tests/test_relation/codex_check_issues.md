# relation 关系层重建记录（Stage 1：executor 的前置）

范围：`relation/{relation_defs.h, key_prefix.h, cursor.{h,cpp}, table.{h,cpp},
kv_catalog.{h,cpp}}`，并用 `storage/mock_engine` 做端到端验证。

目标（你拍板的方案）：数据 key 用 `@data/<len><db>/<len><table>/` + 保序主键
编码；行值整行一个 blob；`relation` 只保留 **KVCatalog + Table 视图 + Cursor**，
`Database`/`DatabaseManager` 这类"活对象"删掉（它们只是元数据的视图，
缓存反而带来一致性问题）。

验证：`test_relation` 32 用例 / 301 断言 / 0 失败；`ctest` 5/5 通过；
`-Wall -Wextra` 0 告警。

---

## 一、关键契约（写进代码注释）

| 内容 | 约定 |
|------|------|
| 数据 key | `@data/<len><db>/<len><table>/` + `KeyCodecs::to_key(pk, 主键列类型)` |
| 行 value | `Row::serialize(schema)` —— 整行一个 blob（带格式版本 v1） |
| 元数据 key | `@system/databases` / `@system/tables/<db>` / `@system/schema/<db>/<table>` |
| 名单序列化 | `[1 字节版本][重复: 4 字节 LE 长度 + 名字]`（长度前缀 framing） |
| 名字大小写 | key 里统一用小写（`Identifier::lower()`）；标识符比较本来就是大小写不敏感 |
| 扫描 | `KeyRange`（逻辑，含开闭/NULL/±∞）→ `to_str_key_range(pk_type)` → 加表前缀 → `kv::KeyRange` |
| 整表边界 | `[prefix, prefix_end(prefix))`，`prefix_end` 把最后一个字节 +1 |

三个"必须"的理由：

1. **主键必须走 KeyCodecs**：`to_string()` 拼 key 会让 `10 < 9`，扫描顺序、
   区间边界、`ORDER BY 主键 + LIMIT` 早停全部失效。测试
   `KeyPrefix.DataKeyIsOrderPreserving` 守住这条。
2. **名字长度前缀**：`@system/db/<db>/schema/<table>` 这种裸分隔符形式，
   名字里带 `/` 或 `,` 就串键。测试 `Catalog.NamesWithSeparatorsDoNotBreakMetadata`
   用 `weird,db` / `a,b/c` 守着。
3. **元数据名单不能逗号拼接**：旧实现用 `std::ostringstream << ","`，
   名字带逗号就解析错。现在有版本号 + 长度前缀。

## 二、结构变化

| 旧 | 新 | 原因 |
|----|----|------|
| `DatabaseManager`（含 database_cache_） | `KVCatalog : sql::Catalog` | 直接实现 `sql_types/catalog.h` 的接口；元数据全部读 KV，没有内存缓存假象（测试 `MetadataSurvivesNewCatalogInstance` 验证） |
| `Database`（活对象 + table_cache_） | 删除 | 它的职责（表列表/schema）已经落在 KVCatalog 的元数据里 |
| `Table`（有 key_prefix_ 成员 + 打印） | `Table` 轻量视图（无缓存、无打印） | 随手创建销毁；`open_table()` 返回它给执行器用 |
| `TableCursor`（`optional<Row>` 无错误通道） | 保留 `Cursor` 抽象 + `error()` | 客户端的唯一取数接口；`next()==nullopt` 时用 `error()` 区分"正常结束"和"出错" |
| 各处 `std::cerr/std::cout` | 全部改为 `std::expected<_, RelError>` | 库代码不打印；错误码 + 上下文信息作为值返回 |

错误码见 `relation/relation_defs.h`：`NOT_OPEN / TABLE_NOT_FOUND / NOT_FOUND /
SCHEMA_ERROR / PRIMARY_KEY_NULL / PRIMARY_KEY_MISMATCH / COLUMN_NOT_FOUND /
KV_ERROR`。

## 三、顺手修掉的存储层真 bug（MockIterator 无视 range 边界）

`MockIterator::seek_to_first()` 原来是"整库的 begin/end"，**完全忽略
`KeyRange::start/end`**：有界区间构造出来的迭代器 `valid()` 立刻为 false，
于是"扫一个区间"永远返回 0 行。`LevelDBIterator::seek_to_first()` 是正确的
（`Seek(start)`；反向 `Seek(end)` 后若 `key >= end` 再 `Prev()`），所以这是
mock 与 leveldb 行为不一致，不是设计问题。

修法：`seek_to_first()/seek()/seek_to_last()` 都补齐边界处理（反向扫描排掉
"正好等于排他上界"的那条 key）。这条 bug 首先被 `test_relation` 抓到
（区间扫描 0 行、`DROP DATABASE` 删不掉 schema），随后 `KVCatalog::remove_prefix`、
`Table::scan` 也都依赖它。

顺带说明：反向扫描的排他上界如果正好是库里的真实 key，
`SeekForPrev(end)` 会停在它上面，必须再退一步 —— mock 和 leveldb 现在都处理了。

## 四、测试覆盖（tests/test_relation）

| 文件 | 用例 | 覆盖 |
|------|------|------|
| `test_key_prefix.cpp` | 6 | 长度前缀、保序（`2 < 10`、负数、NULL 最小、字符串字节序）、表前缀、`prefix_end` 精确性、系统/数据 key 区分 |
| `test_table.cpp` | 16 | 点查/不存在/主键 NULL、全表正反扫描、半开区间、单点区间、非 NULL 全表、空区间、**行损坏报错**、schema 校验失败、NOT NULL 列写 NULL、部分列更新、主键不匹配、未知列、删除、清空、字符串主键顺序 |
| `test_catalog.cpp` | 10 | 建库/重名/大小写不敏感、建表/读回 schema/重名/无主键被拒、元数据跨实例可见、名字含分隔符、open_table 视图、DROP TABLE 连数据一起删、DROP DATABASE 连数据一起删、USE 是会话状态、引擎未打开报 NOT_OPEN |

## 五、遗留 / 下一步（Stage 2：executor）

- 执行器还没写：`Scan → Filter → Sort(TopN) → Limit → Projection` 的算子与
  `ExecutorFactory`（当前 `executor/` 下的草稿引用 `plan_->key_set`、`sql::Table`、
  `Status` 等已不存在的东西，20 处编译错误，需要重写）。
  - 计划：`exec::Executor` 直接继承 `sql::Cursor`（客户端只需要一种取数接口），
    再加 `open()/close()/plan()`；`ScanExecutor` 组合多个 `Table::scan(range, dir)`
    （降序 = 从后往前遍历区间数组），并按 `exclude_keys` 归并跳过。
- `kv_factory.cpp` / `leveldb_engine` 还没进 CMake（新构建只用 mock，
  接 leveldb 需要 `../Debug/include`、`../Debug/lib`）。
- 主键以外的排序（Sort/TopN 的比较器）、多区间合并扫描的早停测试，都在 Stage 2。

---

## 六、Cursor 接口收敛与搬家（按你的决定）

### 1. `sql::Cursor` 移到 `sql_types/cursor.h`

理由：它是"结果行流"的抽象，与存储方式无关 —— 存储扫描（`TableCursor`）是
一个实现者，执行器的根算子（排序/过滤/投影之后的最终行流）是另一个实现者；
客户端只依赖 `sql_types` 就能拿到这个接口，不必依赖存储层。

### 2. 接口收敛成"只有所有实现者都成立"的成员

```cpp
class Cursor {
 public:
  virtual std::expected<Row, CursorError> next() = 0;  // 结束 = CursorErrorCode::END
  virtual void close() = 0;                            // 幂等，释放底层资源
};
```

| 旧成员 | 处理 | 原因 |
|--------|------|------|
| `next() -> optional<Row>` | 改成 `expected<Row, CursorError>` | 结束用 `END` 表达（按你的决定统一），不用再让调用方"看返回值 + 再看 error()"两条通道 |
| `error()` | 删除 | 错误已经在 `next()` 的返回值里；连续调用会**粘住**同一个错误，漏看一次也不会把错误当结束 |
| `valid()` | 删除 | 要预读一行才知道答案，和错误状态纠缠；"还有没有"直接看 `next()` |
| `reset()` | 下沉到 `TableCursor` | 只有存储扫描有"从头再扫一遍"的能力 |
| `range()` | 下沉到 `TableCursor` | 存储概念，结果游标没有 |
| `close()` | 新增 | 客户端可能中途丢弃游标，必须能确定性释放 KV 迭代器/排序缓冲 |

`Table::scan()/scan_all()` 现在返回 `std::unique_ptr<TableCursor>`：
既保留 `reset()/range()` 的可用性，又能当作 `Cursor` 用（unique_ptr 自动转换）。

### 3. 错误类型的分层（一个小决定，需要你确认）

行流错误 `CursorError`（含 `END`）放在 `sql_types/cursor.h`，**relation 与
executor 共用**；`executor/exec_defs.h` 的 `ExecError` 只管"执行这件事本身"
失败（打不开、计划不支持、内存超限……），不再重复一个 `END`。

原因：`sql_types` 不能反向依赖 `executor`，而存储游标和算子都要用同一套
"流结束"语义，所以这个类型必须住在 `sql_types`。如果你更希望 executor 侧
只有一个 `ExecError`（把 `END` 也放进去），那就得把它也搬到 `sql_types`
（或让 exec 依赖 relation），我按你的选择改。

### 4. 新增测试（`Cursor` 套件，4 条）

- `EmptyScanEndsNormallyInsteadOfErroring`：空区间第一次 `next()` 就是 `END`，
  再调用仍是 `END`；`END` 不算错误（`is_error() == false`）。
- `ErrorIsStickyAcrossCalls`：行损坏 → `SCHEMA_ERROR`；**接着调用还报同一个错误**，
  不会被误当成正常结束。
- `CloseIsIdempotentAndEndsTheStream`：`close()` 调两次安全，之后 `next()` 返回 `END`。
- `ResetRescansTheSameRange`：`TableCursor::reset()` 回到区间起点；`range()` 仍可用。
