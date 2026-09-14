# SQL Engine with LevelDB Backend

A small SQL engine written from scratch: hand-written type system and schema,
Flex/Bison front end, primary-key-ordered range scans, a Volcano-style executor,
a pessimistic single-writer transaction model, and LevelDB underneath
(plus an in-memory Mock engine used for tests and development).

中文版：[README.md](README.md)

```
SQL text
  │ parser/        Flex+Bison -> AST (source positions & highlighting come from here)
  │ statement/     AST -> sql::Query (structural conversion + semantic validation)
  │ planner/       rewrite -> optimize (PK ranges + residual filters + cost model) -> plan tree
  │ executor/      Volcano operators -> ResultCursor (client sees sql::Cursor)
  │ relation/      Catalog / Table / Cursor (table views and metadata)
  │ storage/       KVStore + KVEngine (connections), TxBuffer, Mock / LevelDB engines
  └ session/       ties it together: one SQL statement -> one cursor
```

## Requirements

- C++23 compiler (this repo is verified with `clang++-mp-23`)
- CMake >= 3.20, Flex, Bison >= 3.0 (`/usr/local/opt/bison/bin/bison`)
- LevelDB dev library, fmt; readline (for the CLI)

## Build and test

```bash
cmake -S . -B build
cmake --build build -j4
ctest --test-dir build --output-on-failure
```

`Makefile` is only a thin wrapper over that CMake setup
(`make build` / `make test` / `make storage-test` / ...).
Artifacts: `build/lib*.a`, `build/sqldb` (CLI), `build/run_tests/test_*`
(one executable per module).

Tests live in `tests/test_<module>/`. **Every test that does not depend on an
engine-specific feature runs on both Mock and LevelDB** — the two engines must
behave identically.

## Usage

```bash
./build/sqldb                          # in-memory engine, interactive
./build/sqldb --engine=leveldb --path=./mydb
./build/sqldb -e "SELECT * FROM users LIMIT 3;"
./build/sqldb script.sql               # run a script
```

```
shop> CREATE TABLE users (id INT PRIMARY KEY, name VARCHAR(16) NOT NULL);
OK
shop> INSERT INTO users (id, name) VALUES (1, 'a'), (2, 'b');
OK, 2 rows affected
shop> SELECT id, name FROM users WHERE id >= 1 ORDER BY id DESC;
id  name
--  ----
2   b
1   a
(2 rows)
```

Supported: `CREATE/DROP DATABASE`, `CREATE/DROP TABLE`, `SELECT`
(`WHERE`/`ORDER BY`/`LIMIT`/`OFFSET`), `INSERT` (multi-row `VALUES`),
`UPDATE`, `DELETE`, `USE`, `EXPLAIN [ANALYZE]`, and transactions
`BEGIN/COMMIT/ROLLBACK` (aliases `START TRANSACTION` / `END` / `ABORT`).
See `client/README.en.md` and the per-module READMEs for details.

Two front ends (same REPL and protocol, different transports):

```bash
./build/sqldb                # local: in-process engine (scripts / embedded / offline)
./build/sqldb-client --host=127.0.0.1 --port=5433   # remote: talks to sqldb-server
```

## Documentation

Each module's `README.md` / `readme.md` is the up-to-date description of that
module (scope, interfaces, known gaps, pitfalls). English mirrors sit next to
them as `*.en.md`; **the Chinese files are the source of truth** — update both
when behavior changes.

| Directory | Contents |
|-----------|----------|
| `parser/README.md` | lexer/grammar, AST nodes, support matrix, reserved words |
| `sql_types/README.md` | type system, Value/Key encoding, KeyRange (logical ranges) |
| `statement/README.md` | AST -> Query conversion and semantic validation |
| `planner/readme.md` | rewrite / optimize / plan tree / cost model |
| `executor/readme.md` | Volcano operators, result cursor, EXPLAIN stats |
| `storage/kv_engine/readme.md` | KVStore/KVEngine, transaction buffer, engine parity |
| `client/README.md` | both clients (local `sqldb` / remote `sqldb-client`): usage, meta commands, EXPLAIN output |
| `server/README.md` | the server `sqldb-server`: artifacts (`build/svr/{bin,etc}`), config, **logging**, routing |
| `raft/DESIGN.md` | **design doc**: the Multi-Raft plan, the two frozen decisions, sharding/transaction rules, phased plan; the P0 core is implemented |

Per-module change logs (design trade-offs and pitfalls) live in
`tests/test_<module>/codex_check_issues.md`.
