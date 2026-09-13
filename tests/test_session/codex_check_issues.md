# session 模块实现记录（端到端驱动）

范围：`session/session.h`、`session/session.cpp`（旧 session 是另一个世界的代码：
include 了已不存在的 `relation/database_manager.h`、`query/...`，整文件重写），
测试 `tests/test_session/`。

目标（按你的要求）：`session::execute(sql) -> sql::Cursor`。

验证：`test_session` 21 用例 / 109 断言 / 0 失败；`ctest` 7/7 通过
（sql_types / parser / statement / planner / relation / executor / session）；
`-Wall -Wextra` 0 告警。

---

## 一、定位

```
execute(sql)
   |
   |  Parser -> StatementBuilder -> StatementValidator -> QueryRewriter
   |         -> Optimizer -> Planner -> ExecutorFactory -> ResultCursor
   v
std::expected<std::unique_ptr<exec::ResultCursor>, SessionError>
```

Session 持有**连接级状态**：一个 KV 引擎 + `KVCatalog`（含"当前数据库"，
即 USE 语义）；流水线的每一段都是无状态对象，随用随建。

返回类型统一成"一个游标"：

- SELECT 类：`ResultCursor`（惰性，第一次 `next()` 才启动根算子）；
- INSERT/UPDATE/DELETE：`execute()` 里就跑完，`cursor->affected_rows()` 读行数，
  游标 `next()` 直接 END；
- DDL / USE：不进计划器（没有行流），直接落到 Catalog，返回 `exec::empty_result()`
  —— 为此在 exec 里加了一个 `EmptyExecutor`（20 行），换来"一条 SQL 一个游标"
  的调用约定。

## 二、错误类型

`SessionError{code, message, sql, span}`：

| 码 | 含义 |
|----|------|
| `EMPTY_SQL` | 空语句（含只有空白/只有分号） |
| `PARSE_ERROR` | 词法/语法 |
| `BUILD_ERROR` | AST -> Query |
| `VALIDATE_ERROR` | 语义校验（库/表/列不存在、类型不匹配、重名……） |
| `REWRITE_ERROR` / `OPTIMIZE_ERROR` / `PLAN_ERROR` | 三段各自的失败 |
| `EXECUTE_ERROR` | 执行/落库失败（含引擎未打开） |
| `NOT_SUPPORTED` | 该语句类型这里不处理 |

两个可读性细节：

- `to_string()` 只在信息后面加一次 `(line L:C)`；
- `highlight(colors)` 用 `stmt::highlight_span` 把出错片段标红
  （高亮实现仍然只有一份：消费 lexer 的 token 流，见 statement/sql_highlight.h）。

实际效果（probe 输出）：

```
SQL: SELECT * FROM userz
  [4] table not found: shop.userz (line 1:15)
  SELECT * FROM [红]userz[/红];
SQL: SELCT 1
  [2] syntax error, unexpected TOK_IDENT (line 1:1)
  [红]S[/红]ELCT 1;
```

## 三、顺手接上的 parser 位置信息（发现的缺口）

lexer 早就用 `YY_USER_ACTION` 维护了 `yylloc`（`sql.l` 里给 AST span 用），
但 `yyerror` 只把行号**拼进 message 文本**（`"line N: syntax error"`），
`ParseError` 的 `line/column` 字段永远是 0 —— 于是上层拿不到位置，无法高亮。

改动：

- `ParserGlobals` 记录第一个错误的 `(line, column, end_line, end_column)`，
  `yyerror` 从 `yylloc` 取（`%locations` 已经开着）；
- `Parser::parse()` 失败时构造 `ParseError(line, column, message)`
  —— **message 恢复成纯文本**，位置走结构化字段；
- 两处 parser 测试的断言从"message 里找 line 4"改成"`error->line == 4`
  且 `to_string()` 里有 line 4"。

这样 session/CLI 才能画出 `^` 和红色片段。

## 四、测试（21 用例）

| 文件 | 用例 | 覆盖 |
|------|------|------|
| `test_pipeline.cpp` | 10 | 建库→USE→建表→INSERT→SELECT 全链路；WHERE+ORDER BY+LIMIT；INSERT/UPDATE/DELETE 后可见性；DROP TABLE 连带清数据、重建后为空；DROP DATABASE（含删当前库后语句报错）；USE 切库；**带/不带分号、结尾空白**；写游标无行 + affected_rows；DDL 返回空游标；坏行在 SELECT 时通过游标报 `SCHEMA_ERROR` |
| `test_errors.cpp` | 11 | 空语句（含纯空白/纯分号）；语法错误带位置 + 高亮片段；表/列不存在是 `VALIDATE_ERROR` 且定位到名字；没有 USE 时报错；USE 不存在的库；建重名表；INSERT 缺 NOT NULL 列；**一次 execute 多条语句必须报错**（不能被悄悄忽略）；引擎未打开 |

`session_test_util.h` 里的 `bootstrap()` 用纯 SQL 建库建表塞数据 —— 测试本身就
是端到端用例，不依赖任何"走后门"的 C++ 调用。

## 五、已知缺口 / 下一步

- **一调一句**：`execute()` 只执行一条语句；多语句（脚本）需要 `parse_multi`
  支持，或者上面再包一层循环。现在多语句是 parse error，不会静默只跑第一条。
- **CLI 还没接**：`main.cpp` / `cmdline/` 仍是旧世界代码；接上 session 后
  就能得到"REPL + 语法高亮 + 错误定位"的完整体验（session 已经把这两样准备好了）。
- **Bison 错误信息里的 token 名**（`unexpected TOK_IDENT`）对用户不友好，
  可以在 `sql.y` 用 `%token TOK_IDENT "identifier"` 这类别名改成自然语言，
  这属于 parser 的润色。
- **事务/批量**：Session 目前一次一条语句，没有 BEGIN/COMMIT（KV 层有
  `begin_transaction` 接口，将来可以在 session 层暴露）。
- **统计信息**：旧 session 有 `query_count/update_count`，新版没做（没有需求，
  而且状态查询会和"错误不进对象状态"的约定打架，真要做应该显式记录到 stats 结构里）。

---

## 六、CLI 接入（cmdline/main.cpp）

`make cli` -> `build/sqldb`：交互 REPL / `-e` / 脚本文件 / 管道输入四种入口，
全部走 `session::execute()`。CLI 只做"读入 → 交给 session → 打印"。

实现要点：

- **语句切分在 CLI**：按顶层 `;`（跳过引号里的分号）切开，一次一条喂给 session；
  同时记下每条语句在原文本里的起始 `(line, column)`，打印错误前把 E 换算成
  **文件绝对位置**（脚本第 6 行出错就报 line 6，而不是块内的 line 2）。
- **结果表格**：列名来自 `exec::ResultCursor::columns()`（这一项是给客户端新加的
  结果元信息：有投影节点用投影列/别名，否则用表 schema 的列）；
  写语句打 `OK, N rows affected`，DDL/USE 打 `OK`
  （exec 的 `Executor::is_write()` 区分这两类）。
- **颜色**：默认跟着 stdout 是不是 TTY；`--color/--no-color` 可强制。
- **readline**：能找到就用（历史/行编辑），否则退回 `std::getline`。
- **leveldb 接进 CMake**：`find_path/find_library` 找到就编
  `storage/leveldb_engine + kv_factory`，CLI 默认用它（`--path` 指定数据目录，
  默认 `./sql_db`），找不到就只用 mock。
  实测跨进程持久化：第一次 `INSERT`，第二次进程 `SELECT` 能读到。

### 又发现的两个 parser 缺口（都已修）

1. **lexer 没有注释规则**：`-- 行注释` 会被当成减号 → 语法错误；
   `lex_tokens.h` 的注释却写着"空白与注释不会产生 token"。
   现在 `sql.l` 支持 `--` 行注释与 `/* */` 块注释（含跨行），不产生 token
   （高亮器按"token 之间的空隙"原样复制，注释照常显示）。
   新增 parser 用例 `SqlStatements.LineAndBlockCommentsAreSkipped`。
2. （上一节已记）语法错误位置之前只拼在 message 文本里，`ParseError.line/column`
   恒为 0 —— 现在结构化上报，CLI 才能画出高亮。

### CLI 冒烟测试

`ctest` 里两个用例直接跑真实可执行文件，只看退出码：

- `cli_smoke_ok`：`cli_smoke.sql`（建库建表 + 增删改查，含注释）→ 期望 0；
- `cli_smoke_fail`：`cli_smoke_fail.sql`（最后一条查不存在的表）→ 期望非 0。

---

## 七、EXPLAIN（只出计划，不执行）

> **已过时（保留当时的设计记录）**：这一节写的是"EXPLAIN 是 session 的文本前缀"。
> 后来 `EXPLAIN`/`ANALYZE` 进了语法层、输出改成单列结果集，
> 见 **第十四节**；`Session::explain()` 与 `strip_explain_prefix()` 都已删除。

入口：`session::explain(sql)` 与 CLI 的 `EXPLAIN <stmt>`。

### 1. 放在哪一层

**session 层的能力，不是语法层的新语句类型**：
`EXPLAIN` 是给客户端看的 introspection，做成 `QueryType` 会牵动
AST/Builder/Validator/Planner/Executor 一大串；而 session 本来就有完整流水线。
代价是 parser 不认识 `EXPLAIN`，所以由 session/CLI 在**文本层**摘前缀
（大小写不敏感、要求是独立单词），并把它记下的列偏移换算回原始文本，
保证报错时高亮的还是用户写的那一行。

为了让 execute/explain 不重复流水线，把 session 的内部拆成了两段：

```
prepare(statement)   : 解析 + AST->Query + 语义校验      （两者共用）
build_plan(query,..) : 重写 -> 优化 -> 生成计划树        （两者共用）
execute()            : prepare + build_plan + open_table + exec::execute
explain()            : prepare + build_plan + plan_tree_to_string（不执行）
```

### 2. 输出就是 `plan_tree_to_string`

计划树本来就是可读的（每个算子一行，按 child 缩进），所以 EXPLAIN 不需要
另写一套格式化。字段含义见 `planner/readme.md`。

### 3. 测试（`Explain` 套件 11 用例）

- 计划形状：全表 `FullScan`、点查询 `IndexScan [5, 5]`、稀疏 IN `RangeUnion
  [[1, 1], [3, 3], [5, 5]]`、`id != 5` 的 `exclude={5}` + Filter 兜底、
  `ORDER BY 主键` 无 Sort 且 `desc`、非主键排序出 `TopN(n=3)` + Limit、
  写语句的 `Update/Delete/Insert` 链。
- 前缀：可选、大小写不敏感、多余空白；带前缀与不带前缀输出完全相同。
- **不执行**：`EXPLAIN DELETE/UPDATE/INSERT` 之后，表里还是原来 3 行且值没变
  （这条用例直接钉住"EXPLAIN 只读"）。
- 错误：DDL/USE → `NOT_SUPPORTED`；表不存在 → `VALIDATE_ERROR` 且
  `highlight()` 能在**原始文本**（含 EXPLAIN 前缀）上正确地框出表名；
  语法错误 → `PARSE_ERROR`；只有 `EXPLAIN` → `EMPTY_SQL`。
- CLI：`cli_smoke.sql` 里加了一条 EXPLAIN，冒烟测试同时覆盖它。

---

## 八、元命令 `\l` / `\dt` / `\d` 与表统计信息

需求：看有哪些 database、每个库里有哪些表、每张表的 schema，以及
**行数与创建时间**这类统计。

### 1. 统计信息要落库（新增元数据）

行数可以现算，但"创建时间/最后写入时间"必须存起来。新增两种 key
（见 `relation/key_prefix.h`，形状与 schema 刻意不同，按前缀删不会误伤）：

```
@system/dbstats/<db>                  -> [1B 版本][8B created][8B last_write]
@system/tablestats/<db>/<table>       -> 同上
```

固定 17 字节的小记录，不用名单那套 framing。
`KVCatalog` 的时间源**可注入**（`KVCatalog::Clock`），默认系统时间，
测试里给个假时钟就能精确断言。

- `create_database` / `create_table` 写入 created_at；
- `drop_table` / `drop_database` 连同统计一起删（否则重建同名表会读到旧值）；
- `touch_table()` 由 **session** 在"写语句真的改了行"之后调用
  （SELECT、影响 0 行的 DELETE 都不更新）—— 这是"最后写入时间"的语义。
- **行数不落库**：`Session::tables()` 用 `Table::row_count()` 现算，
  避免计数器写放大与漂移；代价 O(n)，元命令场景可以接受（在文档里写明）。

### 2. session 暴露元信息查询

```cpp
struct DatabaseInfo { Identifier name; int64_t created_at; size_t table_count; bool is_current; };
struct TableInfo    { Identifier name; size_t column_count; Identifier primary_key;
                      int64_t created_at, last_write_at; size_t row_count; };

std::vector<DatabaseInfo> databases() const;                      // \l
std::vector<TableInfo>    tables(Identifier db = {}) const;       // \dt / \d
std::optional<TableSchema> table_schema(Identifier table, Identifier db = {}) const;  // \d <表>
```

db 参数为空 = 当前库（client 不用自己判断会话状态）。

### 3. CLI：元命令与逐行处理

反斜杠开头的整行是元命令：`\l` / `\dt [库]` / `\d [库.]表` / `\c <库>` / `\?`。
为了让元命令能插在脚本中间，`run_text()` 从"整段按 `;` 切"改成**逐行**处理：

```
逐行：
  行首是 '\' 且当前没有未完成的语句 -> 当元命令执行
  否则累积到当前语句；遇到 ';' 就执行
```

这样元命令行不参与语句累积（也不会因为缺 `;` 把后面的内容吞掉），
同时脚本行号仍是文件绝对行号。
顺带把结果表格打印抽成 `print_grid()`（SELECT 结果与元命令共用），
并且所有 stderr 输出前先 `fflush(stdout)`，管道/重定向下顺序才正确。

### 4. 测试

| 位置 | 覆盖 |
|------|------|
| `test_relation/test_catalog.cpp`（+5） | 假时钟下建库/建表时间精确；`touch_table` 只改 last_write；跨实例可读；DROP TABLE / DROP DATABASE 清掉统计（重建同名表不残留旧值）；对不存在的库表报 NOT_FOUND / TABLE_NOT_FOUND |
| `test_session/test_metadata.cpp`（+7） | `databases()` 的名字/表数/建库时间/当前库标记；`tables()` 的列数/主键/行数/建表时间/最后写入时间；SELECT 与 0 行写不更新时间、真写才更新；指定别的库列表；`table_schema()` 三种情况；DROP TABLE 后元信息同步；空会话不报错 |
| `cli_smoke.sql` | 脚本里混入 `\l` / `\dt` / `\d t` / `\dt smoke`，冒烟测试顺带覆盖 |

---

## 九、EXPLAIN ANALYZE（真跑一遍，报实际行数/耗时）

### 1. 执行器加统计：NVI 包装，平时零开销

`Executor` 的 `open/next/close` 改成**非虚包装**，内部调算子的
`open_impl/next_impl/close_impl`：

```cpp
ExecError open() { if (report_) report_->on_open(plan()); return open_impl(); }
auto next()     { auto r = next_impl();
                  if (report_) r ? report_->on_row(plan()) : report_->on_finish(plan());
                  return r; }
void close()    { close_impl(); if (report_) report_->on_finish(plan()); }
```

好处：数行数/记时间**只写一处**，9 个算子只管干活；不传 `ExecReport` 时
算子内部没有任何计数器（只有一个空指针判断），普通执行的成本不变。
`ExecReport` 以 `plan::PlanNode*` 为身份登记 `NodeStats{rows, start_us, end_us}`，
节点数很少，线性查找足够。

### 2. 输出：`exec::explain_text(plan, &report)`

计划树每行后面挂 `[rows=N time=T]`，就是 `plan_tree_to_string` 的加强版
（report 为空时退化成纯计划树，普通 EXPLAIN 复用它）。

### 3. 安全边界

**只允许 SELECT**：写语句会真的改数据，`EXPLAIN ANALYZE DELETE ...` 直接
返回 `NOT_SUPPORTED`（用例里同时断言数据没被动过）。要分析写语句，
应该等事务支持后包在 `ROLLBACK` 里 —— 这是 Postgres 的老坑。

### 4. 测试（Explain 套件 18 用例，其中 7 条是 ANALYZE）

- 实际行数：`id >= 4` 的 IndexScan `rows=2`；`age >= 30` 的 Filter `rows=3`
  （全表 5 行、过滤后 3 行，选择率一眼可见）；
- **早停可证明**：不带 LIMIT 的扫描 `rows=5`，带 `ORDER BY pk LIMIT 2` 的
  扫描 `rows=2`；
- 显式 `analyze=true` 与 `EXPLAIN ANALYZE` 前缀都支持；不带 analyze 时
  输出里没有 `[rows=]`；
- 写语句被拒绝且数据不变；执行期错误（坏行）会报 `EXECUTE_ERROR`；
  空结果集也照常打印计划与 `rows=0`。

---

## 十、成本模型（占位）：`planner/cost.h` + 一个决策点

你的要求是"只做第一步，因为这 cost model 只是占位用，展示一个存在的环节"，
所以这里刻意做得最小、最好解释：

### 1. 环节怎么接进去的

```
@system/tablestats（session 维护行数）
        │  StatsProvider（std::function，planner 不依赖具体 Catalog）
        v
   planner/cost.h  CostModel ── optimizer：点集 vs 全表扫的决策
        │                     └─ planner  ：给每个计划节点标 cost（EXPLAIN 展示）
        v
   EXPLAIN 的 [cost=startup..total] + EXPLAIN ANALYZE 的 [rows=.. time=..]
```

- **唯一的决策点**（`optimizer.cpp`）：稀疏点集要不要真的下推。
  `CostModel::point_lookups(k)` vs `filter(full_scan(N), N)`；
  常数 `seek = 10`（一次随机读 ≈ 扫 10 行），所以大约 `k > N/5` 时退化。
- **正确性红线**：退化时必须把主键谓词放回 `remaining_filter`
  （走的是"翻译失败"那条已有的回退路径），否则会"少扫了但结果不对"。
  测试 `CostModel.FallbackKeepsResultsIdentical` 直接对比两种表大小的结果集。
- **统计缺失就不决策**：`RelationStats.rows_known == false` 时保持原规则行为；
  planner/optimizer 两个测试套件用的都是"无统计"的 Optimizer，
  所以它们仍然在测纯规则路径（两种路径都有覆盖）。
- **对比口径一致**：全表扫一侧要算上"每行都过一遍谓词"的过滤成本，
  和 EXPLAIN 显示的成本口径一致 —— 否则会选出"看起来更贵"的计划。

### 2. 实跑效果

```
-- 3 行表：点集相对表太大 -> 退化全表扫（谓词回退到过滤条件）
shop> EXPLAIN SELECT * FROM users WHERE id IN (1, 3);
Filter(id IN (1, 3))  [cost=0.0..6.0]
  FullScan(users pk=id INT, asc)  [cost=0.0..3.0]

-- 100 行表：同样的 SQL，点查更便宜
shop> EXPLAIN SELECT * FROM users WHERE id IN (1, 3);
RangeUnion(users pk=id INT, [[1, 1], [3, 3]], asc)  [cost=20.0..22.0]
```

`EXPLAIN ANALYZE` 同一条会给出实际值，例如 3 行表那条：
`Filter ...[rows=2 ...] / FullScan ...[rows=3 ...]`（扫 3 行、过滤出 2 行）。

### 3. 测试

- `test_session/test_cost.cpp`（5 条）：小表退化成全表扫且谓词在 Filter 里；
  大表仍然是 RangeUnion；**两种计划的结果集逐值相同**；计划里带 `cost=`；
  行数由写语句维护（+INSERT / -DELETE / UPDATE 不动）。
- `test_session/test_explain.cpp`：`RangeUnionForSparseInList` 改成 100 行表
  （否则会被成本模型正确地退化成全表扫）。

### 4. 明确不做（你的要求）

直方图/NDV、选择率估计、二级索引与 join 的候选搜索、成本参数的可配置化
—— 都留给以后；现在这一步只是把"统计 → 成本 → 选计划"的位置标出来。

---

## 十一、事务第一步：悲观单写者（语句原子性 + 回滚免费）

按你的定案，只做"方案 A + 全程持写锁"的第一片，细节写在
`storage/kv_engine/readme.md`。这里只记结论、接口修正与踩到的坑。

### 1. 接口修正（你指出的那处）

覆盖层查询**不用** `std::optional<std::optional<ByteValue>>`：

```cpp
struct OverlayOp {
  enum class Kind : uint8_t { kNone, kValue, kTombstone };
  Kind kind = Kind::kNone;
  ByteValue value;          // 仅 kValue 有效
};
OverlayOp TxBuffer::lookup(const Key&) const;
```

三个状态各有名字：`kNone`（下层回答）/ `kValue`（本事务的新值）/
`kTombstone`（本事务删了）。点读与扫描（`TxIterator`）共用同一套判定。
`OverlayCursor`（给合并迭代器用的有序游标）与显式 BEGIN/COMMIT 一起做，
现在放进来会是死代码。

顺带把同类问题统一了：`Table::find` 从 `expected<optional<Row>, RelError>`
改成 `expected<Row, RelError>`，不存在用 `RelErrorCode::NOT_FOUND` 表达 ——
和游标用 `CursorErrorCode::END` 表达"结束"是同一套约定（状态用错误码，
不用两层套娃）。

### 2. 落地内容

- `kv::WriteBatch`：新增 `remove_range`（整段删）与 `sync` 标志；
- `TxBuffer`：有序 op log（提交顺序）+ 覆盖视图（读），带 64MB 上限；
- 两个引擎（Mock / LevelDB）都实现 `begin/commit/rollback`：写进缓冲、
  读穿、扫描过滤覆盖、提交时一次 WriteBatch（`sync=true`）；
- session：每条写语句/DDL 用 `AutoCommit` 守卫包成**一个事务**
  （提前 return 自动回滚）=> 语句原子性；
- `Table::truncate` / `KVCatalog::remove_prefix` 改用 `remove_range`
  （简化 + 事务里只占一条 op）。

### 3. 踩到的坑（两个，都写进了测试）

1. **`TxIterator::normalize()` 用错了方向**：`Iterator::next()` 的定义就是
   "按扫描方向前进"（反向迭代器内部是 `--`），我却按 `direction` 去调
   `prev()` —— 反向扫描直接扫不到任何东西（`ReverseScanAlsoAppliesTheOverlay` 抓到）。
   现在 `TxIterator` 干脆不要 direction 参数，统一 `next()`。
2. **LevelDB 侧提交死锁**：`commit_transaction()` 持着 `mutex_` 又调
   `write_batch()`（同一个非递归 mutex）—— Mock 引擎当时已经绕开了，
   LevelDB 没有 —— CLI 用 leveldb 跑第一次就挂住。
   修法：两个引擎都抽出 `apply_batch_locked()`（"调用方必须已持锁"），
   commit 走它；并补 `TxLevelDb` 三条**引擎一致性**用例（同一个语义在两个
   引擎上各跑一遍）—— 这类 bug 只有"两个引擎都测"才拦得住。

### 4. 测试

| 位置 | 用例 | 覆盖 |
|------|------|------|
| `tests/test_tx`（新，14 条） | 11 条 Mock + 3 条 LevelDB | 缓冲对 DB 不可见/提交后可见、回滚一切照旧、读自己的写、正反向扫描的覆盖、单写者 `Busy`、空提交/空回滚 `NotFound`、**提交失败一条都不落且缓冲可重试**、`remove_range` 顺序敏感、一批一条非法 op 整体拒绝 |
| `tests/test_session/test_transaction.cpp`（新，4 条） | | 写语句失败不留半截写且之后可用、**DDL 失败不留"schema 有/名单没有"的半成品**、提交后数据与行数统计一致、SELECT 不占写锁 |

当前：sql_types 209 / parser 70 / statement 51 / planner 65 / relation 41 /
executor 40 / session 55 / tx 14，加 2 个 CLI 冒烟测试，`ctest` 10/10，
`-Wall -Wextra` 0 告警；leveldb + CLI 跨进程持久化实测通过。

### 5. 下一步

（已做，见第十二节。）

---

## 十二、显式多语句事务：BEGIN / COMMIT / ROLLBACK + 合并迭代器

### 1. 接口/能力

- session 层关键字（和 EXPLAIN 一样在**文本层**识别，不动语法层）：
  `BEGIN` / `COMMIT` / `ROLLBACK`，外加标准别名 `START TRANSACTION` /
  `END` / `ABORT`；大小写、结尾分号、前后空白都认。
- CLI 元命令 `\begin` / `\commit` / `\rollback`（就是给这三个语句加个外壳），
  事务中提示符加 `*`（psql 习惯）。
- `Session::in_transaction()` 暴露会话级事务状态；`AutoCommit` 守卫在显式事务里
  **让位**（不新开、不提交），由 COMMIT/ROLLBACK 收尾。
- 事务里语句失败 -> 事务标记中止（`tx_failed_`）：后续语句报
  `TRANSACTION_ERROR`，只能 ROLLBACK；此时 COMMIT 实际执行回滚并明确报错。
  校验期错误（还没写任何东西）**不中止**事务。

### 2. 合并迭代器（`MergingIterator` + `OverlayCursor`）

自动提交时"过滤 + 覆盖值"就够了；多语句事务里 `BEGIN; INSERT; SELECT; COMMIT`
的 SELECT 必须看到**DB 里还不存在**的新插入行，所以扫描变成归并：
覆盖游标按 key 顺序把新插入的 key 插进结果流，墓碑把两边一起吃掉，
范围删除靠 `lookup()` 的序号判定。正/反向、`seek`、`seek_to_last`、`prev`
都覆盖了测试。

### 3. 这一轮踩到的四个坑（都进了测试）

1. `TxIterator::normalize()` 方向用错（`next()` 本身已是"按扫描方向前进"）
   -> 反向扫描一行都扫不到；
2. LevelDB 提交死锁（持锁重入 `write_batch`）-> 抽 `apply_batch_locked`；
3. **`OverlayCursor` 存扫描区间的指针** -> `Table::scan` 的临时 `KeyRange`
   一返回就悬垂，表现为"事务内 SELECT 只看到第一行"（两个引擎都中招，
   回归用例是"事务里插 3 行再 SELECT 出 3 行"）；边界现在复制进游标；
4. 合并流的 `prev()`/`seek_to_last()` 不能"各退一格再取最小"——要取
   扫描方向上**更靠后**的那个候选才是"前一项"。

### 4. 测试

| 位置 | 覆盖 |
|------|------|
| `tests/test_tx`（+4，共 18） | 合并迭代器：新插入的 key 出现在正/反向扫描、覆盖已有、删掉的消失、`seek`/`seek_to_last`/`prev` 也能看到覆盖层 |
| `test_session/test_transaction.cpp`（+5，共 9） | BEGIN 后事务内 SELECT 看到自己插的 3 行（回归用例）+ COMMIT 落库；ROLLBACK 撤销全部（插入/删除/更新）；事务控制错误（无事务 COMMIT/ROLLBACK、嵌套 BEGIN）；事务内**执行期**失败 -> 中止 -> 后续语句报错 -> COMMIT 实际回滚且缓冲里的写也没了；`START TRANSACTION`/`END`/`ABORT` 别名与大小写/分号 |
| `cli_smoke.sql` | 脚本里混入 `\begin` / `\rollback` / `BEGIN; COMMIT;` |

当前：sql_types 209 / parser 70 / statement 51 / planner 65 / relation 41 /
executor 40 / session 60 / tx 18，加 2 个 CLI 冒烟测试，`ctest` 10/10，
`-Wall -Wextra` 0 告警；mock 与 leveldb 两个引擎的语义一致（tx 套件两套都跑）。

---

## 十三、事务控制语句回归语法层：BEGIN / COMMIT / ROLLBACK 进 sql.l / sql.y

### 1. 为什么迁移（上一轮的欠账）

第十二节把 `BEGIN/COMMIT/ROLLBACK` 做成了 **session 层的文本匹配**
（`transaction_command()`：把整条语句去掉空白与分号后逐字符比对）。这个做法有三个问题：

1. **词法/高亮不是同一份规则**：`Session::execute("BEGIN")` 走的是字符串比对，
   语法层从来不认识 `BEGIN`；而高亮器是按 `lex_collect_tokens()` 的 token 上色的，
   `BEGIN` 只是一个普通标识符 —— 于是"解析器认、高亮不认"的漂移又出现了；
2. **注释/别名/组合语句绕过检查**：`BEGIN -- 注释` / `BEGIN WORK` /
   `EXPLAIN BEGIN` 这类输入，文本比对要么误判要么给不出有用信息
   （`EXPLAIN BEGIN` 会一路走到 planner 才报"unsupported statement"）；
3. **别名与保留字没有统一出处**：`START TRANSACTION`/`END`/`ABORT` 的归一化
   只存在于 session 的 if 链里，将来任何新前端都要重写一遍。

现在的分工：**语法层认出"是哪一种事务操作"，session 层执行**（会话状态、
单写者锁、中止语义都需要连接状态）。

### 2. 改了什么

| 位置 | 内容 |
|------|------|
| `parser/sql.l` | 新增关键字 `begin/start/transaction/work/commit/end/rollback/abort`（整词匹配，大小写不敏感） |
| `parser/sql.y` | `transaction_stmt`：标准写法 + 别名 → `TXN_BEGIN/TXN_COMMIT/TXN_ROLLBACK`；**不支持的形式给明确原因**（`AND CHAIN`、`ROLLBACK TO SAVEPOINT`、事务模式/隔离级别） |
| `parser/ast.h/.cpp` | `NODE_TRANSACTION` + `TransactionNode{kind}` + make/print/free，并按 `NodeType` 顺序登记进 `nodes[]`（编译期长度检查会兜住漏登记） |
| `sql_types/query.h` | `QueryType::TRANSACTION` + `TransactionKind` + `TransactionStmt{kind}`（进 `QueryStmt` 变体、`stmt_type<>`、`transaction()` 访问器、`is_transaction()`） |
| `statement/` | builder：`NODE_TRANSACTION` → `TransactionStmt`；validator：直接放行（不查 Catalog、不看会话状态） |
| `session/session.cpp` | 删掉 `transaction_command()` 文本匹配，改成 `query->transaction()` 分派到 `begin/commit/rollback_transaction()` |

### 3. 顺序上的一个细节（有意为之）

`BEGIN` 以前在 `prepare()` **之前**就被认出来，所以"事务已中止"的检查在它之后。
迁移后必须先 `prepare()` 才能知道是不是事务语句，于是改成：

```
prepare() ──> 是事务语句？ ──是──> begin/commit/rollback（中止状态下只有它能跑）
                │否
                └──> tx_failed_ ? ──是──> "current transaction is aborted"
```

也就是说**中止的事务里连语法错误也被"事务已中止"盖住**（Postgres 风格）：
否则用户会以为"改一下语法就能继续"，实际上必须 `ROLLBACK`。
这条已经有用例钉住（`AbortedTransactionMasksSyntaxErrorsToo`）。

### 4. 保留字的代价（写进 README 了）

`BEGIN/START/TRANSACTION/WORK/COMMIT/END/ROLLBACK/ABORT` 变成保留字，
不能再当表名/列名（`SELECT commit FROM t` 现在报语法错）。
SQL 标准里 `BEGIN/COMMIT/ROLLBACK/END/ABORT/START` 本来就是保留字，
多出来的是 `TRANSACTION`/`WORK`（PG 里它们是非保留字，这里为了简单也保留了）。

### 5. 测试

| 位置 | 用例 | 覆盖 |
|------|------|------|
| `tests/test_parser/test_transaction.cpp`（新，7 条） | | 四种 BEGIN 写法 / `COMMIT|END` / `ROLLBACK|ABORT`、大小写、`WORK` 后缀、`print_transaction_node` 与语句 span、token 流里是 `TOK_*`（高亮自动跟随）、不支持形式的**具体报错信息**、`BEGINNER` 不是关键字、事务语句后面不能再跟别的语句 |
| `tests/test_statement/`（+2，共 53） | | builder：别名归一化成三种 kind、`target_table()==nullptr`、别的语句上 `transaction()==nullptr`；validator：**没 USE 也放行**（不看库表），但 catalog 没打开仍拒绝 |
| `tests/test_session/test_transaction.cpp`（+4，共 13） | | `BEGIN WORK`/`COMMIT WORK`/`ROLLBACK WORK`（文本层时代这些是语法错误）、报错信息来自语法层（`AND CHAIN`/`SAVEPOINT`/事务模式）、保留字不能当列名、中止事务掩盖语法错误但 `ROLLBACK` 仍可跑、`EXPLAIN BEGIN` 被拒（NOT_SUPPORTED） |

当前：sql_types 209 / parser 77 / statement 53 / planner 65 / relation 41 /
executor 40 / session 64 / tx 18，加 2 个 CLI 冒烟测试，`ctest` 10/10，
`--clean-first` 全量重建 0 告警。

### 6. 下一步

1. **`EXPLAIN` 还没迁**：~~见第十四节~~ —— 已按"产出单列结果集、统一走
   `execute()`"的方案做完，见下面的第十四节。
2. `Session::begin/commit/rollback_transaction()` 目前是 private 成员，
   前端只能通过 `execute()` 进来（`\begin` 元命令就是这么做的）。将来若要在
   连接池/事务管理器里显式调用，再考虑把 `begin_transaction()` 提为公开 API。
3. ~~顺带发现（不在本次范围）：`INSERT` 撞主键**不报错**，是直接覆盖。~~
   已修：见第十四节的第 2 点（现在报 `CONSTRAINT_VIOLATION`，不覆盖旧行）。

---

## 十四、EXPLAIN 进语法层（产出结果集）+ INSERT 主键重复必须报错

### 1. EXPLAIN：从"文本前缀"变成语法层的语句前缀，输出一个结果集

原来的做法（第十三节之前）：CLI 用 `is_explain()` 判前缀、`Session::explain()`
把前缀摘掉再解析，还要把错误的列号**换算回原文**（`restore_original_text`）。
问题：`EXPLAIN BEGIN` 要一路走到最后才报错；`EXPLAIN` 不是关键字，高亮/注释
都不认它；返回类型被迫是 `std::string`，于是 EXPLAIN 只能单独开一个入口，
和"一条 SQL -> 一个游标"的约定分叉。

现在（用户拍板：**产出单列结果集、统一走 `execute()`**）：

| 位置 | 内容 |
|------|------|
| `parser/sql.l` | 关键字 `explain` / `analyze` |
| `parser/sql.y` | `explain_stmt: TOK_EXPLAIN [TOK_ANALYZE] statement` → `NODE_EXPLAIN{analyze, statement}`（前缀包住内层语句，内层节点类型不变） |
| `parser/ast.*` | `NODE_EXPLAIN` + make/print/free（free 递归释放内层） + 注册表 |
| `statement/source_span.cpp` | 收集 span 时钻进 `NODE_EXPLAIN` 的内层（位置始终是原文列号） |
| `statement/stmt_builder.cpp` | builder **不认** `NODE_EXPLAIN`：直接扔进来报 `UNSUPPORTED_AST_NODE`（不是查询，不该被当普通语句执行） |
| `executor` | 新增 `MaterializedExecutor` + `exec::text_result(column, lines)`：把内存里的行当普通结果集吐出去（`plan() == nullptr`） |
| `session` | `prepare()` 返回 `Prepared{query, explain, analyze}`：在 builder 之前拆掉前缀；`execute()` 里 EXPLAIN 先于事务分派处理；`execute_explain()` 生成计划（ANALYZE 才真跑）→ `text_result("QUERY PLAN", lines)` |
| `cmdline` | 删掉 `is_explain()` 与"打印文本"分支：EXPLAIN 和别的语句一样走 `execute()` + 表格输出 |

顺带删掉的：`StrippedExplain` / `match_keyword()` / `restore_original_text()` /
`is_explain()`（约 100 行纯文本处理），以及 `Session::explain()` 这个双入口。

输出形状（psql 风格，单列 `QUERY PLAN`，一行一个算子）：

```
shop> EXPLAIN SELECT id FROM t WHERE id >= 1 ORDER BY id DESC LIMIT 2;
QUERY PLAN
--------------------------------------------------------------
Project([id])  [cost=10.0..11.5]
  Limit(limit=2 offset=0)  [cost=10.0..11.0]
    IndexScan(t pk=id INT, [1, +∞), desc)  [cost=10.0..11.0]
(3 rows)
```

### 2. INSERT 撞主键：报错，不覆盖

`Table::insert` 以前就是一次 `put` —— 主键已存在时**静默覆盖**（实测两条
`INSERT ... VALUES (1, ...)` 都返回 `OK, 1 row affected`，`SELECT` 只剩一行）。
按用户要求改成显式报错，并把"约束不满足"这个语义在每一层各给一个码
（各层本来就各有自己的错误枚举 + 映射函数，不引入新风格）：

| 层 | 变化 |
|----|------|
| `relation` | `RelErrorCode::DUPLICATE_PRIMARY_KEY`；`Table::insert` 先 `engine_->exists(key)`，命中就报错（`duplicate primary key in t: 1`），**不写**。事务里 `exists()` 走覆盖层，所以"同一事务里插两次同 key"立刻被拦下 |
| `sql_types` | `CursorErrorCode::CONSTRAINT_VIOLATION` + `to_cursor_error` 映射 |
| `executor` | `ExecErrorCode::CONSTRAINT_VIOLATION` + `to_exec_error` 映射 |
| `session` | `SessionErrorCode::CONSTRAINT_VIOLATION` + `from_exec_error/from_cursor_error` 映射 |

语义上不做任何假定：不是"后写覆盖先写"，也不是 upsert；要 upsert 是以后
`INSERT ... ON CONFLICT` 的事。删除后可以重新插入同一个主键（有用例）。

### 3. 测试

| 位置 | 用例 | 覆盖 |
|------|------|------|
| `test_parser/test_explain.cpp`（新，5 条） | | 前缀包住语句且内层类型不变、`ANALYZE` 标志、六种内层语句（含 `BEGIN`）、大小写/空白、token 流里是 `TOK_EXPLAIN/TOK_ANALYZE`、`EXPLAIN;`/`ANALYZE ...`/`EXPLAINX` 都报错 |
| `test_executor/test_text_result.cpp`（新，2 条） | | 单列/一行一段/正常结束/空结果集/close 幂等 |
| `test_session/test_explain.cpp`（改，`Explain` 套件 19 条） | | 结果集形状（列名 `QUERY PLAN`、单元格里没有换行）、不加前缀就是真执行、`EXPLAIN BEGIN`/DDL/USE 报 NOT_SUPPORTED、`SELCT` 报语法错、位置/高亮、ANALYZE 的统计与写语句拒绝、ANALYZE 下执行期错误照报 |
| `test_session/test_transaction.cpp`（+2） | | `EXPLAIN BEGIN` 报 NOT_SUPPORTED 且不会真的开事务；**事务中止后 `EXPLAIN` 也被拒**（ANALYZE 会真的读数据），语义与 Postgres 对齐 |
| `test_relation/test_table.cpp`（+2） | | 主键重复报 `DUPLICATE_PRIMARY_KEY` 且旧行原封不动、删除后可重插；**事务里**插两次同 key 立刻报错、回滚后一切照旧 |
| `test_executor/test_write.cpp`（+1） | | SQL 层重复 INSERT -> 报错且行数不变 |
| `test_session/test_errors.cpp`（+1） | | 端到端 `CONSTRAINT_VIOLATION` + 旧值不变 + 换主键立刻可用 |

当前：sql_types 209 / parser 82 / statement 53 / planner 65 / relation 43 /
executor 43 / session 67 / tx 18，加 2 个 CLI 冒烟测试，`ctest` 10/10，
`-Wall -Wextra` 0 告警。

### 4. 下一步

- EXPLAIN 现在走 `execute()`，所以 `EXPLAIN` 的结果将来可以直接喂给
  `ORDER BY`/`WHERE`（真数据库里 EXPLAIN 就是个结果集）—— 目前不支持，
  语法上是语句前缀，不是查询。
- `EXPLAIN ANALYZE` 仍然只允许 SELECT（写语句会真改数据）。等有了
  "包在事务里跑完再回滚"的接口，可以放开。
- 多行 `VALUES` 仍未支持（`INSERT ... VALUES (..),(..)` 解析失败）。
