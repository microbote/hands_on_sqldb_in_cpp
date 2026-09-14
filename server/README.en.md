# server layer: sqldb-server (TCP server)

中文版：[README.md](README.md)

Puts `session::Session` on the wire: one coroutine and one Session per
connection, running on the generic coroutine server framework in
`common/svrkit` (`common/net` is the poller/socket layer).

## Artifacts

```
build/svr/
  bin/sqldb-server        executable
  etc/sqldb-server.conf   commented default config (copied from the source tree)
```

```bash
make server
./build/svr/bin/sqldb-server --config=./build/svr/etc/sqldb-server.conf
./build/svr/bin/sqldb-server --help
```

Sources: `main_server.cpp` (entry), `server.{h,cpp}` (frames + routing),
`config.{h,cpp}`, `logger.{h,cpp}`, `etc/sqldb-server.conf`.

## Command line

```
sqldb-server [--config=PATH] [--listen=HOST:PORT] [-h|--help]
```

`--listen` overrides `server.listen`. Value-taking options currently accept
the `=` form only.

## Configuration

`[section]` + `key = value`, `#`/`;` comments, unknown sections are legal; a
misspelled key inside a known section is rejected by `validate()` at startup
with the field name. See `etc/sqldb-server.conf` (commented) for every key;
only logging is covered here.

## Logging

Two keys, both in `[server]`:

| Key | Meaning |
|-----|---------|
| `log_level` | `error < warn < info < debug`, a **ceiling**, default `info` |
| `log_file` | log file; **empty = stderr** (default) |

To see debug output:

```ini
[server]
log_level = debug
log_file  = ./sql_db/server.log
```

```bash
./build/svr/bin/sqldb-server --config=./build/svr/etc/sqldb-server.conf
tail -f ./sql_db/server.log
```

Line format (identical for both sinks):

```
[2026-09-14 12:39:13.096] [info] listening on 127.0.0.1:5433 (log_level=debug, log_file=./sql_db/server.log)
[2026-09-14 12:39:14.106] [debug] connection accepted
[2026-09-14 12:39:14.109] [debug] connection closed: fd=10
[2026-09-14 12:39:14.621] [info] shutdown: stop accepting new connections
[2026-09-14 12:39:14.646] [info] stopped: connections=1 statements=5 errors=0 rows=1 ...
```

What each level currently emits:

| Level | Content |
|-------|---------|
| `error` | attach failure, connection handler threw |
| `warn` | graceful shutdown found connections still open |
| `info` | startup (including where logs go), shutdown summary + metrics, idle timeouts |
| `debug` | per-connection accept / close |

Write semantics:

- the file is opened `O_WRONLY|O_CREAT|O_APPEND` and **each log line is a
  single `write()`** — POSIX makes the seek-to-end + write of `O_APPEND`
  atomic, so lines never interleave across threads or processes; an in-process
  mutex keeps the fully formatted line together;
- **restarts append**, they do not truncate the previous run;
- failing to open it (missing directory / no permission) is a **startup
  error**, never "logging silently to nowhere";
- a failed write (disk full) drops that one line; logging must not take the
  service down.

## Routing (one connection, one coroutine)

```
svrkit::TcpServer(main thread)   ParseService   WriteService   ReadPool[N]
  └─ connection coroutine ── SQL ──► co_await submit ──► ...
```

`SELECT` outside a transaction → read pool (`execution.read_threads`, truly
concurrent); everything else (including every statement inside a transaction)
→ the write service thread (single writer). Write/parse/read queues are
bounded; when full the statement is rejected as `busy` instead of blocking the
event loop.

## Known gaps

- only the `=` form of `--config=`/`--listen=` is accepted (no space form);
- no log rotation/size cap — use `logrotate` for long-running deployments;
- `statement_timeout_ms` is recorded but not enforced yet (needs cooperative
  checkpoints in the executor).
