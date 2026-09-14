# client 层：两个命令行客户端（本地 / 远程）

English version: [README.en.md](README.en.md)

目录结构（**共享代码只有一份，前端只是薄壳**）：

```
client/
  connection.{h,cpp}   SqlConnection：execute + 元信息 + 当前库/事务状态
                         ├─ LocalConnection  —— 进程内直接包 session::Session
                         └─ RemoteConnection —— 自定义协议（连 sqldb-server）
  repl.{h,cpp}         共用 REPL：语句切分 / 元命令 / 表格 / 错误 caret
  local/main.cpp       → build/sqldb         本地客户端（这条 README 描述的行为）
  remote/main.cpp      → build/sqldb-client  远程客户端（同一套 REPL，加 --host/--port）
```

```bash
./build/sqldb                                        # 本地：进程内引擎
./build/sqldb-client --host=127.0.0.1 --port=5433    # 远程：连 sqldb-server
```

**远程客户端目前只支持远程**（没有 `--engine/--path`）。下面描述的输出格式、
元命令、EXPLAIN 行为**两个前端一致**（同一份代码）；`\l` / `\dt` / `\d` 在远程
下走 META 帧（服务端读 Catalog 后回元信息），`\c` 就是 `USE`。

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

| 选项                         | 说明                                                     |
| ---------------------------- | -------------------------------------------------------- |
| `-e, --execute SQL`        | 执行一条 SQL 后退出                                      |
| `-i, --interactive`        | 强制交互模式                                             |
| `--engine=mock\|leveldb`    | 引擎（默认 leveldb，没编进来就退回内存 mock）            |
| `--path=DIR`               | leveldb 数据目录（默认`./sql_db`，已在 .gitignore 里） |
| `--echo-sql`               | 执行前回显语句（带语法高亮）                             |
| `--no-color` / `--color` | 关/开颜色（默认跟着 stdout 是不是 TTY）                  |

交互模式里输入 `exit` / `quit` / `\q` 退出；语句可以分多行写，遇到 `;` 才执行。

## 快速上手（demo）

### 1. 本地客户端：建库 → 建表 → 插入 → 查询

```console
$ ./build/sqldb                  # 默认 leveldb；没编进 leveldb 就退回内存 mock
sqldb 命令行（local；输入 exit / quit / \q 退出）
(none)> CREATE DATABASE shop;
OK
(none)> \c shop
现在连接的是数据库 "shop"
shop> CREATE TABLE users (
   ...  (用 ';' 结束，空行=立即执行)   id INT PRIMARY KEY,
   ...  (用 ';' 结束，空行=立即执行)   name VARCHAR(32) NOT NULL,
   ...  (用 ';' 结束，空行=立即执行)   age INT
   ...  (用 ';' 结束，空行=立即执行) );
OK
shop> INSERT INTO users (id, name, age) VALUES (1, 'alice', 30), (2, 'bob', 25);
OK, 2 rows affected
shop> SELECT id, name, age FROM users ORDER BY id;
id  name   age
--  -----  ---
1   alice  30
2   bob    25
(2 rows)
```

多行输入：只要还没出现 `;`，提示符就变成 `   ...  (用 ';' 结束，空行=立即执行)`。
**空行 = 立即执行**（相当于替你补一个 `;`），所以敲错东西时按两次回车就能马上
看到报错，不会卡在续行里。

### 2. 看元信息：`\l` / `\dt` / `\d`

```console
shop> \l
Database  Tables  Created
--------  ------  -------------------
* shop    1       2026-09-14 12:11:16

shop> \dt
Table  Rows  Columns  Primary key  Created              Last write
-----  ----  -------  -----------  -------------------  -------------------
users  2     3        id           2026-09-14 12:11:16  2026-09-14 12:11:16

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
      created=2026-09-14 12:11:16  last write=2026-09-14 12:11:16
```

### 3. 事务：`BEGIN` → 改 → `COMMIT` / `ROLLBACK`

进入事务后提示符带 `*`：

```console
shop> BEGIN;
OK
shop*> INSERT INTO users (id, name, age) VALUES (3, 'carol', 35);
OK, 1 row affected
shop*> SELECT id, name FROM users ORDER BY id;   -- 事务内看得到自己未提交的插入
shop*> ROLLBACK;                                 -- 或 COMMIT
OK
shop> SELECT id FROM users ORDER BY id;          -- 回滚后就像什么都没发生
```

也可以用元命令外壳：`\begin` / `\commit` / `\rollback`（等价于上面对应的
SQL 语句）。

### 4. 只看计划：`EXPLAIN`（不执行）

```console
shop> EXPLAIN SELECT id, name FROM users WHERE age >= 30 ORDER BY id DESC LIMIT 2;
QUERY PLAN
-----------------------------------------------------------------
Project([id, name])  [cost=0.0..2.5]
  Limit(limit=2 offset=0)  [cost=0.0..1.0]
    Filter(age >= 30)  [cost=0.0..6.0]
      FullScan(users pk=id INT, desc)  [cost=0.0..3.0]
(4 rows)
```

加 `ANALYZE` 会真跑一遍并报每个算子的实际行数/耗时：
`EXPLAIN ANALYZE SELECT ...`（写语句会被拒绝，因为会真改数据）。

每行的 `[cost=起步..总代价]` 来自 planner 的成本模型（现在是个**占位实现**，
但已经真的参与决策：比如 `WHERE id IN (1,3,5)` 在**小表**上会退回
`FullScan + Filter`，因为"3 次点查"比"扫 3 行"贵；表大了才会变成
`RangeUnion`）。

### 5. 远程：起服务器，再用 `sqldb-client` 连它

```bash
# 终端 1：起服务器（产物在 build/svr/ 下，配置带注释；日志见 server/README.md）
./build/svr/bin/sqldb-server --config=./build/svr/etc/sqldb-server.conf

# 终端 2：远程客户端（和本地客户端是同一套 REPL/元命令/EXPLAIN）
$ ./build/sqldb-client --host 127.0.0.1 --port 5433
remote(127.0.0.1:5433)> \l
remote(127.0.0.1:5433)> SELECT id, name FROM users;
```

服务端日志：`[server] log_level`（默认 `info`，可调 `debug`）+ `log_file`
（空 = stderr）。行格式 `[时间] [级别] 消息`，文件是 append 写。细节见
`server/README.md`。

### 6. 脚本与非交互用法

```bash
./build/sqldb -e "SELECT * FROM users"      # 跑一条就退出
./build/sqldb demo.sql                      # 跑脚本文件（可多个）
echo "SELECT * FROM users;" | ./build/sqldb # 管道输入当脚本
cat demo.sql | ./build/sqldb --echo-sql     # 执行前回显（带语法高亮）
```

脚本里出错会报**文件里的绝对行号**（CLI 会做 span 换算），不是"第几条语句"。

### 7. 交互快捷键（readline）

| 按键            | 作用                                             |
| --------------- | ------------------------------------------------ |
| `↑` / `↓`     | 翻命令历史（历史文件见下）                       |
| `←` / `→`     | 行内移动；`Backspace` / `Delete` 删字符       |
| `Ctrl-A`/`Ctrl-E` | 跳到行首 / 行尾                             |
| `Ctrl-R`        | 反向搜索历史                                     |
| `Tab`           | 补全：SQL 关键字（来源 `sql.l`）+ 当前库表名；`\` 开头补元命令 |
| `Ctrl-C`        | 放弃当前行，重新输入                             |
| `Ctrl-D`        | 退出（空行时）                                   |

历史文件：本地 `~/.sqldb_history`，远程 `~/.sqldb_client_history`（每次启动
load、退出时 save）。补关键字用的大小写形态是小写（和 `sql.l` 的规则同源），
输入也是大小写无关的。

## 元命令

反斜杠开头的行是元命令（独占一行，不参与 SQL 语句累积），交互模式和脚本里都能用：

| 命令                                     | 作用                                                     |
| ---------------------------------------- | -------------------------------------------------------- |
| `\l`                                   | 列出所有数据库：表数量、建库时间，`*` 标出当前库       |
| `\dt`                                  | 列出当前库的表：行数、列数、主键、建表时间、最后写入时间 |
| `\dt <库>`                             | 列指定库的表                                             |
| `\d`                                   | 等价`\dt`（当前库）                                    |
| `\d <表>` / `\d <库>.<表>`           | 看表结构（列/类型/可空/约束）+ 统计                      |
| `\c <库>`                              | 切换当前数据库（等价`USE <库>`）                       |
| `\begin` / `\commit` / `\rollback` | 事务控制（等价`BEGIN` / `COMMIT` / `ROLLBACK`）    |
| `\?`                                   | 元命令帮助                                               |

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

| 信息             | 来源                                                          | 说明                                                                                                                 |
| ---------------- | ------------------------------------------------------------- | -------------------------------------------------------------------------------------------------------------------- |
| 表数量           | 元数据名单                                                    | `@system/tables/<db>`                                                                                              |
| 行数（`\dt`）  | **现算**（全表扫）                                      | 元命令故意不信任计数器：避免漂移；代价 O(n)                                                                          |
| 行数（成本模型） | `@system/tablestats/<db>/<table>`                           | **有**维护的行数：session 在写语句真的改了行之后按受影响行数增量更新（UPDATE 不改行数，只刷新时间）            |
| 建库/建表时间    | `@system/dbstats/<db>`、`@system/tablestats/<db>/<table>` | 记录**v2 = 25 字节**（版本 + created + last_write + rows）；v1（17 字节，没有 rows）是旧数据，读得到但行数未知 |
| 最后写入时间     | 同上                                                          | **session 在写语句真的改了行之后**更新（SELECT、影响 0 行的写都不更新）                                        |
| 列数/主键/结构   | schema                                                        | `@system/schema/<db>/<table>`                                                                                      |

老数据没有统计记录时，时间显示 `-`，不会报错；planner 拿不到行数时
`rows_known = false`，成本模型退回纯规则行为。

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
Project([id, name])  [cost=0.0..2.5]
  Limit(limit=2 offset=0)  [cost=0.0..1.0]
    Filter(age >= 30)  [cost=0.0..6.0]
      FullScan(users pk=id INT, desc)  [cost=0.0..3.0]
(4 rows)

shop> EXPLAIN SELECT * FROM users WHERE id IN (1, 3, 5);
QUERY PLAN
-------------------------------------------------------------------------
RangeUnion(users pk=id INT, [[1, 1], [3, 3], [5, 5]], asc)  [cost=30.0..33.0]
(1 row)

shop> EXPLAIN UPDATE users SET age = 1 WHERE id = 3;
QUERY PLAN
--------------------------------------------------------------------
Update(users pk=id)  [cost=10.0..11.0]
  IndexScan(users pk=id INT, [3, 3], asc)  [cost=10.0..11.0]
(2 rows)
```

说明：每行末尾的 `[cost=起步..总代价]` 是 planner 的成本模型给出的估算，
**它真的参与决策**（现在只是占位实现）。上面 `IN` 的例子成立需要表足够大：
小表上"3 次点查"比"扫 3 行"贵，成本模型会主动退回 `FullScan + Filter`
（正确性不受影响，只是少了一次索引优化）。

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
Project([id])  [rows=2 time=98us cost=10.8..13.3]
  Limit(limit=2 offset=0)  [rows=2 time=93us cost=10.8..11.8]
    TopN(order_by=[age ASC] n=2)  [rows=2 time=95us cost=10.8..11.8]
      Filter(age >= 30)  [rows=2 time=75us cost=0.0..6.0]
        FullScan(users pk=id INT, asc)  [rows=3 time=70us cost=0.0..3.0]
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
  （会报明确原因）。事务是**悲观单写者**：多个连接可以同时开事务（各自拿
  快照做可重复读），但**同时只有一个能写** —— 第二个连接的写语句报 `busy`；
- 表格宽度按字节算，CJK 会略微不齐（要精确得算 East Asian Width）。
