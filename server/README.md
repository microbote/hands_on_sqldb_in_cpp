# server 层：sqldb-server（TCP 服务端）

English: [README.en.md](README.en.md)

把 `session::Session` 挂到网络上：一连接一协程一 Session，跑在
`common/svrkit` 的通用协程框架上（`common/net` 是 poller/socket）。

## 产物与目录

```
build/svr/
  bin/sqldb-server        可执行
  etc/sqldb-server.conf   带注释的默认配置（构建时从源码树复制）
```

```bash
make server
./build/svr/bin/sqldb-server --config=./build/svr/etc/sqldb-server.conf
./build/svr/bin/sqldb-server --help
```

源码：`main_server.cpp`（入口）、`server.{h,cpp}`（协议帧+路由）、
`config.{h,cpp}`、`logger.{h,cpp}`、`etc/sqldb-server.conf`。

## 命令行

```
sqldb-server [--config=PATH] [--listen=HOST:PORT] [-h|--help]
```

`--listen` 覆盖 `server.listen`；两个带值选项目前只认 `=` 写法。

## 配置

`[section]` + `key = value`，`#`/`;` 注释，未知 section 合法；已知 section
里拼错的 key 启动时被 `validate()` 拦下并报字段名。全部字段见
`etc/sqldb-server.conf`（带注释），这里只说日志。

## 日志

两个键，都在 `[server]`：

| 键 | 说明 |
|----|------|
| `log_level` | `error < warn < info < debug`，是**上限**，默认 `info` |
| `log_file` | 日志文件；**空 = 写 stderr**（默认） |

想看 debug：

```ini
[server]
log_level = debug
log_file  = ./sql_db/server.log
```

```bash
./build/svr/bin/sqldb-server --config=./build/svr/etc/sqldb-server.conf
tail -f ./sql_db/server.log
```

行格式（两种接收端一致）：

```
[2026-09-14 12:39:13.096] [info] listening on 127.0.0.1:5433 (log_level=debug, log_file=./sql_db/server.log)
[2026-09-14 12:39:14.106] [debug] connection accepted
[2026-09-14 12:39:14.109] [debug] connection closed: fd=10
[2026-09-14 12:39:14.621] [info] shutdown: stop accepting new connections
[2026-09-14 12:39:14.646] [info] stopped: connections=1 statements=5 errors=0 rows=1 ...
```

各级别现在打了什么：

| 级别 | 内容 |
|------|------|
| `error` | attach 连接失败、连接 handler 抛异常 |
| `warn` | 优雅退出时还有连接没关 |
| `info` | 启动（含日志去哪儿）、退出汇总（metrics）、空闲超时关连接 |
| `debug` | 每条连接的 accept / close |

写入语义：

- 文件用 `O_WRONLY|O_CREAT|O_APPEND` 打开，**每条日志一次 `write()`** ——
  POSIX 保证 `O_APPEND` 的"定位到末尾 + 写"是原子的，所以跨进程/跨线程每行
  不会互相插花；进程内再加一把互斥锁，保证"格式化好的整行"一起写出；
- **重启是追加**，不会清掉上一次的日志（`log_file` 指向同一个文件即可）；
- 打不开（目录不存在/没权限）→ **启动期直接报错退出**，不会静默地"以为在
  记日志"；
- 写失败（磁盘满等）只丢弃那一条，日志不该拖垮服务。

## 路由（一连接一协程）

```
svrkit::TcpServer(主线程)   ParseService   WriteService   ReadPool[N]
  └─ connection 协程 ── SQL ──► co_await 提交 ──► ...
```

`SELECT` 且不在事务里 → 读线程池（`execution.read_threads`，真并发）；
其余一切（含事务里的所有语句）→ 写服务线程（单写者）。写/解析/读队列各有
上限，满了按 `busy` 拒绝当前语句，不阻塞事件循环。

## 已知缺口

- 只有 `--config=`/`--listen=` 的 `=` 写法（空格写法不支持）；
- 日志没有轮转/大小上限，长期跑要自己配 `logrotate`；
- `statement_timeout_ms` 目前只是记下来，真正打断要等执行器有协作检查点。
