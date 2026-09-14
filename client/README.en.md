# client layer: two command-line clients (local / remote)

中文版：[README.md](README.md)

Layout (**the shared code exists once; the front ends are thin shells**):

```
client/
  connection.{h,cpp}   SqlConnection: execute + metadata + current db/tx state
                         ├─ LocalConnection  -- wraps session::Session in-process
                         └─ RemoteConnection -- the custom protocol (sqldb-server)
  repl.{h,cpp}         shared REPL: statement splitting / meta commands / tables / error caret
  local/main.cpp       -> build/sqldb         local client (the behaviour below)
  remote/main.cpp      -> build/sqldb-client  remote client (same REPL, adds --host/--port)
```

```bash
./build/sqldb                                        # local: in-process engine
./build/sqldb-client --host=127.0.0.1 --port=5433    # remote: talks to sqldb-server
```

The remote client is **remote-only** (no `--engine/--path`). Everything described
below — output format, meta commands, EXPLAIN — is identical in both front ends
(same code); over the wire `\l` / `\dt` / `\d` use META frames (the server reads
the Catalog and sends the metadata back), and `\c` is just `USE`.

The CLI does exactly three things: **read SQL -> hand it to
`session::Session` -> print the result**. It knows nothing about
parser/planner/executor internals, and errors come straight from
`SessionError` (message + position + highlighted snippet).

## Usage

```bash
make cli                     # build build/sqldb (alias of `make sqldb`)
./build/sqldb                # interactive (the default when stdin is a TTY)
./build/sqldb -e "SELECT * FROM t"          # run one statement and exit
./build/sqldb script.sql [more.sql]         # run scripts
echo "SELECT 1" | ./build/sqldb             # piped input is treated as a script
```

Options:

| Option | Meaning |
|--------|---------|
| `-e, --execute SQL` | run one SQL statement and exit |
| `-i, --interactive` | force interactive mode |
| `--engine=mock\|leveldb` | engine (default leveldb; falls back to the in-memory mock if not built in) |
| `--path=DIR` | LevelDB data directory (default `./sql_db`, gitignored) |
| `--echo-sql` | echo each statement before running it (syntax highlighted) |
| `--no-color` / `--color` | disable/enable colors (default follows whether stdout is a TTY) |

Type `exit` / `quit` / `\q` to leave interactive mode. Statements may span
multiple lines; nothing runs until `;`.

## Quick start (demos)

### 1. Local client: create db -> create table -> insert -> query

```console
$ ./build/sqldb                  # leveldb by default; falls back to in-memory mock
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

Multi-line input: until a `;` shows up the prompt turns into
`   ...  (用 ';' 结束，空行=立即执行)`. **An empty line means "run it now"**
(it stands in for the missing `;`), so a typo never leaves you stuck in the
continuation prompt — two Enters and you see the error.

### 2. Metadata: `\l` / `\dt` / `\d`

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

### 3. Transactions: `BEGIN` -> write -> `COMMIT` / `ROLLBACK`

The prompt gets a `*` while a transaction is open:

```console
shop> BEGIN;
OK
shop*> INSERT INTO users (id, name, age) VALUES (3, 'carol', 35);
OK, 1 row affected
shop*> SELECT id, name FROM users ORDER BY id;   -- sees your own uncommitted insert
shop*> ROLLBACK;                                 -- or COMMIT
OK
shop> SELECT id FROM users ORDER BY id;          -- gone, as if it never happened
```

Meta-command shells work too: `\begin` / `\commit` / `\rollback`.

### 4. Plan only: `EXPLAIN` (does not execute)

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

`EXPLAIN ANALYZE SELECT ...` really runs the query and reports actual rows and
time per operator (write statements are rejected — they would change data).

`[cost=start..total]` comes from the planner's cost model (a **placeholder**
for now, but it already makes real decisions: `WHERE id IN (1,3,5)` falls back
to `FullScan + Filter` on a **small** table because 3 point lookups cost more
than scanning 3 rows; only a big table turns it into `RangeUnion`).

### 5. Remote: start the server, connect with `sqldb-client`

```bash
# terminal 1: start the server (artifacts live under build/svr/, config is commented)
./build/svr/bin/sqldb-server --config=./build/svr/etc/sqldb-server.conf

# terminal 2: remote client (same REPL / meta commands / EXPLAIN)
$ ./build/sqldb-client --host 127.0.0.1 --port 5433
remote(127.0.0.1:5433)> \l
remote(127.0.0.1:5433)> SELECT id, name FROM users;
```

Server logging: `[server] log_level` (default `info`; set `debug` for more) plus
`log_file` (empty = stderr). Lines look like `[time] [level] message` and the
file is append-only. See `server/README.md`.

### 6. Scripts and non-interactive use

```bash
./build/sqldb -e "SELECT * FROM users"      # one statement, then exit
./build/sqldb demo.sql                      # run a script file (repeatable)
echo "SELECT * FROM users;" | ./build/sqldb # piped input is a script
cat demo.sql | ./build/sqldb --echo-sql     # echo each statement (highlighted)
```

In scripts an error reports the **absolute line number in the file** (the CLI
maps spans), not "statement #N".

### 7. Interactive keys (readline)

| Key | Action |
|-----|--------|
| `Up` / `Down` | command history (history files below) |
| `Left` / `Right` | move in the line; `Backspace` / `Delete` edit |
| `Ctrl-A` / `Ctrl-E` | jump to start / end of line |
| `Ctrl-R` | reverse search history |
| `Tab` | completion: SQL keywords (from `sql.l`) + current-db table names; `\` completes meta commands |
| `Ctrl-C` | abandon the current line |
| `Ctrl-D` | exit (on an empty line) |

History files: `~/.sqldb_history` (local), `~/.sqldb_client_history` (remote) —
loaded at startup, saved on exit. The completion list is lowercase (same source
as `sql.l`); input itself is case-insensitive.

## Meta commands

A line starting with a backslash is a meta command (it stands alone and does not
join the SQL buffer); it works interactively and from scripts:

| Command | Effect |
|---------|--------|
| `\l` | list databases (table count, creation time, `*` marks the current one) |
| `\dt` | list tables of the current database (rows, columns, PK, created, last write) |
| `\dt <db>` | list tables of another database |
| `\d` | same as `\dt` |
| `\d <table>` / `\d <db>.<table>` | table structure (columns/types/nullability/constraints) + stats |
| `\c <db>` | switch the current database (= `USE <db>`) |
| `\begin` / `\commit` / `\rollback` | transaction control (= `BEGIN` / `COMMIT` / `ROLLBACK`) |
| `\?` | meta command help |

Transactions can also be written as SQL: `BEGIN [WORK|TRANSACTION]` /
`COMMIT [WORK]` / `ROLLBACK [WORK]`, plus the standard aliases
`START TRANSACTION` / `END` / `ABORT` (case-insensitive). **Those keywords are
recognized by the grammar** (`transaction_stmt` in `parser/sql.y`); `\begin` is
just a thin shell around the same statements. While a transaction is open the
prompt adds a `*` after the database name:

```
shop> \begin
transaction started
shop*> INSERT INTO users (id, name, age) VALUES (1, 'a', 10);
OK, 1 row affected
shop*> SELECT * FROM users;          -- sees its own uncommitted insert
id  name  age
--  ----  ---
1   a     10
(1 row)
shop*> \rollback
rolled back
shop> SELECT * FROM users;           -- nothing happened after all
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

shop> \d users
Table "users"
+--------+---------------+----------+--------------+
| Column| Type         | Nullable| Constraint  |
|--------|---------------|----------|--------------|
| id    | INT          | NO      | PRIMARY KEY |
| name  | VARCHAR(32)  | NO      | NOT NULL    |
| age   | INT          | YES     |             |
+--------+---------------+----------+--------------+
stats: rows=2  columns=3  primary key=id
       created=2026-09-13 08:49:45  last write=2026-09-13 08:49:45
```

The data comes from `session::Session::databases() / tables() /
table_schema()` (see `DatabaseInfo` / `TableInfo` in `session/session.h`).

## Where the statistics come from

| Info | Source | Note |
|------|--------|------|
| table count | metadata list | `@system/tables/<db>` |
| row count (`\dt`) | **computed on demand** (full scan) | the meta command deliberately does not trust the counter: it never drifts; cost O(n) |
| row count (cost model) | `@system/tablestats/<db>/<table>` | a **maintained** counter: the session updates it by the affected-row delta after a write statement that really changed rows (UPDATE keeps the count, it only refreshes the timestamp) |
| database/table creation time | `@system/dbstats/<db>`, `@system/tablestats/<db>/<table>` | record **v2 = 25 bytes** (version + created + last_write + rows); v1 (17 bytes, no rows) is legacy data that still reads, with an unknown row count |
| last write time | same | updated **by the session only when a write statement really changed rows** (SELECTs and zero-row writes do not update it) |
| column count / PK / structure | schema | `@system/schema/<db>/<table>` |

Old data without a stats record simply shows `-` instead of failing; when the
planner has no row count, `rows_known = false` and the cost model falls back to
pure rule-based behaviour.

## EXPLAIN

`EXPLAIN <SELECT|INSERT|UPDATE|DELETE>` prints the plan tree and **does not run**
the statement. `EXPLAIN`/`ANALYZE` are **grammar-level keywords**
(`explain_stmt` in `parser/sql.y`) and go through the same
`session.execute()`; the output is a **single-column result set** (column
`QUERY PLAN`, one operator per row), so the CLI needs no special branch.
DDL/USE/transaction statements have no plan and report `NOT_SUPPORTED`:

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
```

The trailing `[cost=start..total]` on each line is the planner's cost model
estimate, and it **really drives decisions** (it is a placeholder for now).
The `IN` example above needs a big enough table: on a small table "3 point
lookups" costs more than "scan 3 rows", so the cost model deliberately falls
back to `FullScan + Filter` (correctness is unaffected, you just lose one
index optimization).

How to read a plan (matching the planner's contract):

- `ORDER BY <primary key>` -> **no Sort node**, only the scan direction becomes
  `desc` (reverse iteration);
- sorting with LIMIT -> `TopN(order_by=[...] n=OFFSET+LIMIT)`;
- `RangeUnion` concatenates multiple ranges (point sets / discrete ranges);
  `exclude={...}` is the skip hint for `<>`/`NOT IN` (the Filter still keeps a
  fallback predicate);
- write statements are chains too:
  `Update/Delete -> Filter? -> RangeUnion? -> Scan`.

### EXPLAIN ANALYZE

`EXPLAIN ANALYZE <SELECT ...>` **really runs the query** and reports actual row
counts and elapsed time per operator (write statements really modify data and
are rejected):

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

`FullScan rows=3` proves three rows were read before sorting; switching to
`SELECT id FROM users ORDER BY id LIMIT 2` shows `FullScan rows=2` — **LIMIT
early stop is provable with numbers**. (The trailing `(6 rows)` is the result
set size: 5 plan lines plus the `(2 rows in result)` line; psql's EXPLAIN has
the same shape.)

Implementation: an executor's `open/next/close` are **non-virtual wrappers**
around `open_impl/next_impl/close_impl`, so counting rows and timing operators
is written once (`exec::ExecReport`); without a report the overhead is a single
null check.

## Output

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
SELECT * FROM [red]userz[/red];
```

- SELECT: an aligned table plus a row count (column names come from the
  projection or the table schema via `ResultCursor::columns()`);
- writes: `OK, N rows affected`; DDL/USE: `OK`;
- errors: a red message plus a snippet produced by `stmt::highlight_span`.

## A small client-side trick: script line numbers

`session::execute()` takes one statement at a time, so parser positions are
relative to *that* statement. While splitting statements on top-level `;`
(skipping semicolons inside quotes) the CLI remembers each statement's starting
`(line, column)` in the original text and shifts the span before printing — so
an error on line 6 of a script reports `line 6`, not `line 2`.

## Known gaps

- Transaction/EXPLAIN keywords (`BEGIN/COMMIT/ROLLBACK/EXPLAIN/ANALYZE/...`)
  are reserved words and cannot be used as table or column names.
- Savepoints (`SAVEPOINT`), `COMMIT AND CHAIN` and isolation-level syntax are
  unsupported (they report a specific reason). Transactions are
  **pessimistic single writer**: several connections may hold transactions at
  once (each with its own snapshot for repeatable read), but only **one can
  write** — a second connection's write statement reports `busy`.
- Table widths are computed in bytes, so CJK text is slightly misaligned
  (exact alignment would need East Asian Width).
