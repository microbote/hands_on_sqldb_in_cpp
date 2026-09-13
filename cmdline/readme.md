# sqldb：命令行客户端

CLI 只做三件事：**读入 SQL → 交给 `session::Session` → 打印结果**。
它不碰 parser/planner/executor 的细节，错误也原样来自 `SessionError`
（信息 + 位置 + 高亮片段）。

## 用法

```bash
make cli                     # 构建 build/sqldb
./build/sqldb                # 交互模式（默认；读 TTY）
./build/sqldb -e "SELECT * FROM t"          # 执行一条后退出
./build/sqldb script.sql [more.sql]         # 跑脚本
echo "SELECT 1" | ./build/sqldb             # 管道输入当脚本
```

选项：

| 选项 | 说明 |
|------|------|
| `-e, --execute SQL` | 执行一条 SQL 后退出 |
| `-i, --interactive` | 强制交互模式 |
| `--engine=mock\|leveldb` | 引擎（默认 leveldb，没编进来就退回内存 mock） |
| `--path=DIR` | leveldb 数据目录（默认 `./sql_db`，已在 .gitignore 里） |
| `--echo-sql` | 执行前回显语句（带语法高亮） |
| `--no-color` / `--color` | 关/开颜色（默认跟着 stdout 是不是 TTY） |

交互模式里输入 `exit` / `quit` / `\q` 退出；语句可以分多行写，遇到 `;` 才执行。

## 元命令

反斜杠开头的行是元命令（独占一行，不参与 SQL 语句累积），交互模式和脚本里都能用：

| 命令 | 作用 |
|------|------|
| `\l` | 列出所有数据库：表数量、建库时间，`*` 标出当前库 |
| `\dt` | 列出当前库的表：行数、列数、主键、建表时间、最后写入时间 |
| `\dt <库>` | 列指定库的表 |
| `\d` | 等价 `\dt`（当前库） |
| `\d <表>` / `\d <库>.<表>` | 看表结构（列/类型/可空/约束）+ 统计 |
| `\c <库>` | 切换当前数据库（等价 `USE <库>`） |
| `\begin` / `\commit` / `\rollback` | 事务控制（等价 `BEGIN` / `COMMIT` / `ROLLBACK`） |
| `\?` | 元命令帮助 |

事务也可以用 SQL 语句写：`BEGIN [WORK|TRANSACTION]` / `COMMIT [WORK]` /
`ROLLBACK [WORK]`，外加标准别名 `START TRANSACTION` / `END` / `ABORT`
（大小写不限）。**这些关键字由语法层识别**（`parser/sql.y` 的
`transaction_stmt`），`\begin` 只是给同一批语句加个外壳：

```
shop> \begin
transaction started
shop*> INSERT INTO users (id, name, age) VALUES (1, 'a', 10);
OK, 1 row affected
shop*> SELECT * FROM users;          -- 事务内能看到自己未提交的插入
id  name  age
--  ----  ---
1   a     10
(1 row)
shop*> \rollback
rolled back
shop> SELECT * FROM users;           -- 回滚后什么都没发生
id  name  age
--  ----  ---
(0 rows)
```

```
shop> \l
Database  Tables  Created
--------  ------  -------------------
* shop    2       2026-09-13 08:49:45
  other   0       2026-09-13 08:49:45

shop> \dt
Table  Rows  Columns  Primary key  Created              Last write
-----  ----  -------  -----------  -------------------  -------------------
users  2     3        id           2026-09-13 08:49:45  2026-09-13 08:49:45
logs   1     2        id           2026-09-13 08:49:45  2026-09-13 08:49:45

shop> \d users
Table "users"
+--------+---------------+----------+--------------+
| Column| Type         | Nullable| Constraint  |
|--------|---------------|----------|--------------|
| id    | INT          | NO      | PRIMARY KEY |
| name  | VARCHAR(32)  | NO      | NOT NULL    |
| age   | INT          | YES     |             |
+--------+---------------+----------+--------------+
统计：rows=2  columns=3  primary key=id
      created=2026-09-13 08:49:45  last write=2026-09-13 08:49:45
```

数据来源：`session::Session::databases() / tables() / table_schema()`
（见 `session/session.h` 的 `DatabaseInfo` / `TableInfo`）。

## 统计信息怎么来的

| 信息 | 来源 | 说明 |
|------|------|------|
| 表数量 | 元数据名单 | `@system/tables/<db>` |
| 行数 | **现算**（全表扫） | 不维护计数器：避免写放大，也不会漂移；代价 O(n) |
| 建库/建表时间 | `@system/dbstats/<db>`、`@system/tablestats/<db>/<table>` | 固定 17 字节记录（版本号 + 两个 int64），由 Catalog 的时间源写入（可注入假时钟做测试） |
| 最后写入时间 | 同上 | **session 在写语句真的改了行之后**更新（SELECT、影响 0 行的写都不更新） |
| 列数/主键/结构 | schema | `@system/schema/<db>/<table>` |

老数据没有统计记录时，时间显示 `-`，不会报错。

## EXPLAIN

`EXPLAIN <SELECT|INSERT|UPDATE|DELETE>` 打印计划树，**不执行**语句。
`EXPLAIN`/`ANALYZE` 是**语法层的关键字**（`parser/sql.y` 的 `explain_stmt`），
和别的语句走同一个 `session.execute()`；输出是**单列结果集**
（列名 `QUERY PLAN`，一个算子一行），所以 CLI 不需要为它单开一个分支。
DDL/USE/事务语句没有计划，会报 `NOT_SUPPORTED`：

```sql
shop> EXPLAIN SELECT id, name FROM users WHERE age >= 30 ORDER BY id DESC LIMIT 2;
QUERY PLAN
-----------------------------------------------------------------
Project([id, name])
  Limit(limit=2 offset=0)
    Filter(age >= 30)
      FullScan(users pk=id INT, desc)
(4 rows)

shop> EXPLAIN SELECT * FROM users WHERE id IN (1, 3, 5);
QUERY PLAN
-------------------------------------------------------------------------
RangeUnion(users pk=id INT, [[1, 1], [3, 3], [5, 5]], asc)
(1 row)

shop> EXPLAIN UPDATE users SET age = 1 WHERE id = 3;
QUERY PLAN
--------------------------------------------------------------------
Update(users pk=id)
  IndexScan(users pk=id INT, [3, 3], asc)
(2 rows)
```

读法（和 planner 的约定一一对应）：

- `ORDER BY 主键` → **没有 Sort 节点**，只把扫描方向变成 `desc`（反向迭代）；
- 有 LIMIT 的排序 → `TopN(order_by=[...] n=OFFSET+LIMIT)`；
- `RangeUnion` 是多区间拼接（点集/离散区间），`exclude={...}` 是 `<>`/`NOT IN`
  的跳点提示（Filter 里仍有兜底谓词）；
- 写语句也是一条链：`Update/Delete -> Filter? -> RangeUnion? -> Scan`。

### EXPLAIN ANALYZE

`EXPLAIN ANALYZE <SELECT ...>` 会**真的把查询跑一遍**，在每个算子后面报实际
行数与耗时（写语句会真改数据，所以这里直接拒绝）：

```sql
shop> EXPLAIN ANALYZE SELECT id FROM users WHERE age >= 30 ORDER BY age LIMIT 2;
QUERY PLAN
--------------------------------------------------------------------------
Project([id])  [rows=2 time=94us]
  Limit(limit=2 offset=0)  [rows=2 time=90us]
    TopN(order_by=[age ASC] n=2)  [rows=2 time=92us]
      Filter(age >= 30)  [rows=2 time=75us]
        FullScan(users pk=id INT, asc)  [rows=3 time=69us]
(2 rows in result)
(6 rows)
```

读法：`FullScan rows=3` 说明排序前确实读了 3 行；把它换成
`SELECT id FROM users ORDER BY id LIMIT 2` 会看到 `FullScan rows=2` ——
**LIMIT 早停是可以用数字证明的**。
（表格最后的 `(6 rows)` 是"结果集有 6 行"，也就是 5 行计划 + 1 行
`(2 rows in result)`；psql 的 EXPLAIN 也是这个形状。）

实现：执行器的 `open/next/close` 是**非虚包装**，里面才调算子的
`open_impl/next_impl/close_impl`，所以"数行数、记时间"只写一处
（`exec::ExecReport`）；不传 report 时零开销，算子实现里只有一个空指针判断。

实现上：语法层把 `EXPLAIN [ANALYZE]` 包成 `NODE_EXPLAIN`，session 在
`prepare()` 里拆掉这层（被解释的语句照常走 builder/validator/planner），
然后复用 `build_plan()`，只是不调执行器（ANALYZE 才真的跑一遍）。
位置信息全程是**原文里的列号**，所以出错时高亮的仍是用户写的那一行。

## 输出

```
shop> SELECT id, name, age FROM users WHERE age >= 30 ORDER BY age DESC;
id  name   age
--  -----  ---
3   carol  35
1   alice  30
(2 rows)

shop> UPDATE users SET age = 26 WHERE name = 'bob';
OK, 1 row affected

shop> CREATE TABLE t (id INT PRIMARY KEY);
OK

shop> SELECT * FROM userz;
table not found: shop.userz (line 1:15)
SELECT * FROM [红]userz[/红];
```

- SELECT：对齐表格 + 行数（列名来自投影列或表 schema，由
  `ResultCursor::columns()` 提供）；
- 写语句：`OK, N rows affected`；DDL/USE：`OK`；
- 错误：红色信息 + `stmt::highlight_span` 出来的高亮片段。

## 一个客户端层的小活：脚本行号

`session::execute()` 一次只吃一条语句，parser 报的位置是"这条语句里"的相对位置。
CLI 在切分语句（按顶层 `;`，跳过引号里的分号）时记下每条语句在原文本里的
起始 `(line, column)`，打印错误前把 span 换算成**文件绝对位置** ——
所以脚本第 6 行出错就报 `line 6`，而不是 `line 2`。

## 已知缺口

- 事务/EXPLAIN 关键字（`BEGIN/COMMIT/ROLLBACK/EXPLAIN/ANALYZE/...`）是保留字，
  不能当表名/列名用；
- 保存点（`SAVEPOINT`）、`COMMIT AND CHAIN`、隔离级别语法都不支持
  （会报明确原因）。事务是**悲观单写者**：第二个连接开事务会 `busy`；
- 表格宽度按字节算，CJK 会略微不齐（要精确得算 East Asian Width）。
- 表格宽度按字节算，CJK 会略微不齐（要精确得算 East Asian Width）。
