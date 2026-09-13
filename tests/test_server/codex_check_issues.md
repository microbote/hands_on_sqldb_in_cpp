# test_server：协程服务器模块的改动记录

`tests/test_server/` 是服务器（`server/` + `client/`）的测试目录。这份文档记录
服务器相关的**设计决策、P0 发现与踩坑**；M1 的测试用例落地后，用例清单也追加
在这里（测试代码本身进 `tests/test_server/`，跑法见 `Makefile` 的 `server-test`）。

现状（2026-09-13）：M1 的服务器骨架 + 客户端层 + 元信息 + 连接生命周期都已
落地（第四 ~ 八节），本轮又补了通用配置包、读线程池与 Poller 抽象（第九节）。
`test_server` 39 用例 / `ctest` 12/12 全绿，干净重建 0 告警，TSan 0 竞态。
第一节是动工前的 P0 体检（parser 线程安全），当时"代码还没开始写"。

---

## 一、P0：parser 不是线程安全的（发现 + 修复）

### 1. 怎么发现的

设计协程服务器时，第一件要确认的事是"同一条流水线能不能多线程跑"。
逐模块扫全局状态，发现**词法/语法层是进程级单例**：

| 全局状态 | 位置 | 说明 |
|---|---|---|
| flex 的扫描缓冲、`yytext/yyleng/yylloc/yylineno` | `parser/sql.l`（非重入） | 扫描器状态全在文件级全局里 |
| `g_parsed_ast` | `parser/ast.cpp:39` | 由语法动作 `set_parsed_ast($$)` 写 |
| `g_parser_state`（`yyerror` 的错误接收方） | `parser/parser.cpp:40` | 构造 `Parser` 时就登记 `instance = this` |
| `lex_collect_tokens()` | `parser/sql.l` | 高亮/工具用的 token 流导出，同样非重入 |

流水线其余部分（statement/planner/executor/relation/session）扫下来没有可变全局，
所以**只有解析这一步**需要处理。

### 2. 证据：先写测试，跑在未修的代码上必崩

新增 `tests/test_parser/test_thread_safety.cpp`（8 线程 × 300 次 × 2 个用例）：

```
$ ./build/run_tests/test_parser        # 未修版本
fatal flex scanner internal error--end of buffer missed      ← 每次必崩
```

两个用例的角度：① 每个线程解析 `... FROM t<N> ...` 并断言 AST 里的表名是
**自己**那份（串状态立刻抓到）；② 一半线程一直解析坏语句、一半解析好语句，
好语句不能因为别人的语法错误而失败。

### 3. 修了什么（两处竞态，第二处是 TSan 补抓的）

| # | 竞态 | 修法 |
|---|------|------|
| 1 | flex 全局态 + `g_parsed_ast` + `g_parser_state` | `Parser::parse()` 里一把**进程级锁**包住"登记错误接收方 → `yyparse` → 取走 AST → 摘掉接收方"；`g_parser_state.instance` 只在解析期间登记（RAII），不再在构造/析构里设（否则别的线程的 `yyerror` 会写进你的对象） |
| 2 | `ast_debug`：解析线程写、**别的线程**在 `free_ast` 的 `DEBUG()` 里读 | 改成 `std::atomic<int>`（读端 `load(relaxed)`） |

顺带：`Parser::reset()` 也纳入同一把锁；内部拆出不加锁的 `clear_error_state()`
给 `parse()` 用（避免自锁）。

### 4. 结果与边界

- `test_parser` 84 → **86 用例**，重复运行稳定；`ctest` 11/11；`--clean-first` 0 告警；
  **ThreadSanitizer 下 0 竞态**。
- 边界写进 `parser/README.md`（中英）：`parse()` 线程安全；
  **`lex_collect_tokens()`（高亮）仍非重入 → 服务端不做高亮**，只把
  `span + 原文` 回给客户端渲染（`stmt::highlight_span` 客户端已有的代码路径）。
- 真并行解析（M3）的可重入化清单：bison `%define api.pure full` +
  `%parse-param`/`%lex-param`；flex `%option reentrant`（`yyscan_t` +
  `yylex_init_extra`）；`set_parsed_ast` 的全局改成 `ctx->result`；
  `lex_collect_tokens()` 每次调用自建扫描器；然后删掉那把锁。

### 5. 备选方案（本轮讨论）：把解析做成**服务线程**

即：session 协程把 SQL 交给一个**专门的 parser 线程**后挂起，parser 线程解析完
再把结果（AST 或 ParseError）扔回来 resume。相比内部加锁：

| | 内部加锁（当前实现） | parser 服务线程 |
|---|---|---|
| 正确性 | 依赖"该锁的都锁了" | 天然单线程，没有共享 |
| 读线程是否被解析阻塞 | 会（等锁） | 不会（协程挂起，线程继续跑别的协程） |
| 额外开销 | 一次 lock/unlock | 两次跨线程唤醒（可批量摊薄） |
| 顺带的收益 | — | 解析配额/超时、将来 SQL→计划缓存都挂在这里 |
| 额外线程 | 无 | 1 条（大部分时间空闲） |

两者不冲突：**锁保留**（给 CLI/测试/工具这些直接调用方兜底），服务端再走
"服务线程"的路子。这也是服务器里"写队列"的同一套机制（见第二节）。

---

## 二、服务器方案的已定决策

| # | 决策 |
|---|------|
| 1 | 协程自研：C++20 `<coroutine>` + `poll`（~~M3 换~~ kqueue/epoll 已提前落地，见第九节 Poller 抽象）+ 跨线程唤醒 pipe；**不用 ucontext**（arm64 macOS 不可用） |
| 2 | **协议：自定义**（帧 = `[u8 type][u32 len][payload]`；`HELLO/QUERY/COLUMNS/ROW/OK/ERROR/PING/BYE`；**NULL 带标志位**；错误回 `span` 不回高亮；`HELLO` 带协议版本）。**不用 protobuf**：消息集小且稳定，协议的难点在流式/关联/取消（protobuf 帮不上），而可读性（nc/tcpdump）在主流程更重要；编解码收在 `protocol` 接口后面，将来换 payload 编解码（含 protobuf）不动 framing 与 server 核心。（环境里 `protoc 29.3` + protobuf 头/库可用，所以这是选择而非限制。）MySQL 协议复杂（握手/capability 协商/认证插件/length-encoded 编码/错误号映射，且 span 没地方放），做成以后可加的适配层 |
| 3 | 事务路由：`SELECT` 且不在事务 → 读池；**事务里的所有语句 + 所有写语句 → 写线程队列**（协程挂起等待，完成后回原线程 resume） |
| 4 | 读线程数可配（`read_threads`，默认 1） |
| 5 | 把 `cmdline` 的 REPL 与 `split_statements()` 抽到 `client/`，`sqldb`（本地）与 `sqldb-client`（远程）共用同一套交互循环；transport 抽象（`SqlConnection`：execute + 元信息），本地实现直接包 `session::Session`，**本地 CLI 不改成网络模式** |
| 6 | **解析做成"服务线程"**：session 协程把 SQL 交给专门的 parser 线程后挂起，parser 线程解析完把结果（AST 或 ParseError）扔回来 resume —— 与"写队列"共用同一套 `ServiceThread` 抽象（1 线程 + 有界队列 + 完成回调 + 回到 owner 线程 resume）。好处：读线程不被解析阻塞、解析有天然配额/超时挂点、将来 SQL→计划缓存的落点。`Parser::parse()` 内部的锁**保留**给 CLI/测试/工具兜底（服务线程上几乎不竞争），M3 可重入化后再删 |

环境事实（选型依据）：x86_64 macOS 11、**无 Boost / 无 libuv**、
`protoc 29.3` + protobuf 头/库**可用**（见决策 2 的取舍）。

`Session` 侧的配套改动：把"parse + build + validate"拆开
（`parse()` 上服务线程，`build+validate` 留在会话线程），见第三节第 0 项。

---

## 三、M1 待办（下一步）

0. `session`：拆出 `parse()` / `execute_parsed()`（解析上服务线程的前提），
   现有 `execute()` 语义不变（CLI/测试继续走它）
1. `server/config`（INI 子集 + 校验，配置错误启动即失败）
2. `server/task.h` + `server/loop`（协程 Task + 每线程 EventLoop + 定时器 + 跨线程唤醒）
3. `server/protocol`（帧编解码 + 自测）
4. `server/service.h`（`ServiceThread`：parse 服务线程 + 写线程共用）
5. `server/net` + `server/connection` + `server/server` + `main_server`（可跑的 `sqldb-server`）
6. `client/repl` 抽取 + `client/connection`（`SqlConnection`：本地/远程）+ `sqldb-client`
7. `tests/test_server`：配置解析、协议编解码、`ServiceThread`、双客户端 E2E（默认 mock 引擎）、优雅退出

验收：A `BEGIN;INSERT;` 后 B 读不到、A `COMMIT` 后 B 读到；A 只读事务期间 B 能写；
NULL 列与错误 span 正确；并发压测下 `ctest` 全绿。

---

## 四、M1 第一切片：服务器骨架 + 端到端跑通（已落地）

代码：`server/`（config / task / loop / service / protocol / server / main_server）
+ `sqldb-server` 可执行 + `tests/test_server`（21 用例，`ctest` 12/12）。

### 1. 这一片做了什么

| 文件 | 内容 |
|---|---|
| `server/config.{h,cpp}` | INI 子集（`[server]/[storage]/[execution]/[session]`）；**未知键 = 拼错了，启动即失败**（带行号） |
| `server/task.h` | 协程基础设施：`Task`（只做 `Task<void>`，结果用引用参数回传，省掉一整套模板坑）、`WaitFd`（poll 水平触发）、`SleepFor` |
| `server/loop.{h,cpp}` | 每线程事件循环：`poll` + 定时器 + 跨线程唤醒管道 + `post/spawn`（协程只在 I/O、服务交接、定时器三处让出） |
| `server/service.h/.cpp` | `ServiceThread` + `SubmitToService`：**parse 服务线程与写线程共用**（有界队列、完成回 owner Loop resume、支持 fire-and-forget） |
| `server/protocol.{h,cpp}` | 自定义帧：`HELLO/QUERY/COLUMNS/ROW/OK/ERROR/PING/BYE`；**NULL 带标志位**；错误带 `span + 原文`（客户端渲染 caret）；16MB 帧上限 |
| `server/server.cpp` | acceptor 协程 + 连接协程：HELLO → 收帧（半帧/粘包）→ **解析上 parse 服务线程** → 按路由执行（只读且不在事务 → 本线程；其余 → 写服务线程）→ 回 COLUMNS/ROW*/OK/ERROR |
| `session` | 拆出 `parse()` / `execute_parsed()` / `parse_error_to_session_error()`（解析上服务线程的前提），`execute()` 语义不变 |

### 2. 这一片踩到的坑（两个都是"必须记住"的）

1. **唤醒管道必须非阻塞**：`Loop` 的 wakeup pipe 原来是阻塞的，drain 到"读不到为止"
   时最后一次 `read()` 会**永久阻塞** → 整个事件循环挂死（现象：服务线程已经把结果
   post 回来，主循环却卡在 poll 之后不再前进）。修法：pipe 两端都设 `O_NONBLOCK`。
   这条现在有 `Loop.TimersAndPostedActionsRun` 与 `ServiceThread.*` 守着。
2. **协程测试不要在协程里依赖 CHECK 失败后继续**（框架的 CHECK 只记账不 return，
   但如果哪天改成 return，`loop.stop()` 就会被跳过而挂死）——现在测试里把
   `stop()` 放在断言之后、并且用"结果先收集、退出后断言"的写法。

### 3. 端到端已验证（`test_server` 里 6 条真起服务 + 连 socket 的用例）

- 建表 / 多行 INSERT（受影响行数 3）/ `SELECT ... ORDER BY` 返回列名与行值；
- **NULL 与空串、字符串 'NULL' 三者可分**（协议 NULL 标志位的意义）；
- 错误帧带 `span + 原文`，且**出错后连接继续可用**（长连接）；
- 两条连接的事务可见性：A `BEGIN;INSERT;` 时 B 读不到，A `COMMIT` 后 B 读到；
- **只读事务不挡写者**：A `BEGIN` 拿快照后 B 的 `UPDATE` 成功，A 再读仍是旧值（可重复读），A `COMMIT` 后看到新值；
- 4 条连接并发插入 + 汇总查询。

### 4. 环境注意

- 在受限沙箱里 `bind()` 会被拒（`Operation not permitted`）：E2E 用例会打印
  `[skip]` 并跳过，不算失败；本机（普通终端）跑就是全绿。
- `make server` / `make server-test` 已加到 Makefile；直接跑：
  `./build/sqldb-server --config=your.conf`。

### 5. 还没做（M1 剩余）

1. ~~`client/` 抽取~~ 见第五节（REPL + `SqlConnection` + `sqldb-client` 已落地）。
2. ~~远程的 `\l` / `\dt` / `\d`~~ 见第六节（META 帧已落地）。
3. ~~连接超时（idle / idle-in-transaction）、优雅退出~~ 见第七节（已落地并有
   用例钉住）；`statement_timeout` 还没做（要等 M3 的协作检查点才能真正打断）。
4. ~~日志与 metrics~~ 见第七节（已落地）；~~`read_threads > 1`~~ 见第九节
   （读线程池已落地，纯读语句在 N 条读线程上真并发）。

---

## 五、客户端层：REPL 与 transport 解耦（已落地）

```
client/
  connection.h/.cpp   SqlConnection（execute + 元信息 + 当前库/事务状态）
                        ├─ LocalConnection  —— 进程内直接包 session::Session
                        └─ RemoteConnection —— 自定义协议（阻塞式同步客户端）
  repl.h/.cpp         共用 REPL：语句切分 / 元命令 / 表格 / 错误 caret
  README.md           客户端层文档（两个前端共用）
  local/main.cpp      → build/sqldb         本地客户端（原 cmdline/，行为不变）
  remote/main.cpp     → build/sqldb-client  远程客户端（--host/--port）
```

目录按"共享 / 本地 / 远程"三分（2026-09-13 调整）：`client/` 是共享库（无 main），
两个前端各自一个薄壳 main。远程客户端**只支持远程**；如果以后想"一个二进制两种模式"
（`sqldb --host=...`），只要在本地 main 里按参数选 transport 即可（共享层两种都实现了）。

- **本地 `sqldb` 行为没有变化**（`OK` / `OK, N rows affected` / 表格 / 报错 +
  caret 全部照旧）：`cli_smoke_ok` / `cli_smoke_fail` 与全部旧套件都绿；它现在
  只是"REPL + LocalConnection"。
- `Outcome` 在连接边界就把游标拉完并物化成"文本 + NULL 标志"，REPL 因此完全
  不知道语句是本地跑的还是远程发的。
- 协议 OK 帧加了 `flags`（bit0 = 在事务里、bit1 = 写语句）与 `current_database`：
  远程客户端的提示符（库名 + `*`）和"OK / OK, N rows affected"的区分都靠它。
- 远程错误：服务端只回 `code + message + 原文 + span`，**客户端**用
  `stmt::highlight_span` 渲染 caret（服务端不碰 lexer，避开非重入）。

### 用例

| 位置 | 覆盖 |
|------|------|
| `tests/test_server/test_client.cpp`（2 条） | `RemoteConnection` 走真协议：DDL / 多行 INSERT（受影响行数）/ SELECT / 错误 span / 事务状态（提示符用）；以及**共用 REPL 跑一段脚本**（输出重定向到临时文件，断言 `OK`、列名、`NULL`、`(2 rows)`）—— 这就是 `sqldb-client` 的代码路径 |

### 环境注意（重复提醒）

受限沙箱里 `bind()` 会被拒：`test_server` 的 E2E 用例会打印 `[skip] ... bind():
Operation not permitted` 并跳过（`ctest` 仍全绿）。**本机终端**跑
`make server-test` 就是全量验证；本会话早前经批准后 6 条真 socket 用例 +
`RemoteConnection` 全部通过。

---

## 六、META 帧：远程客户端也能 `\l` / `\dt` / `\d`

| 位置 | 内容 |
|---|---|
| `server/protocol.{h,cpp}` | 新增帧 `9 kMeta`（`u8 kind, bytes arg1, bytes arg2`）与 `10 kMetaReply`（bytes 载荷）；kind = `kDatabases` / `kTables` / `kSchema`。载荷编解码：库列表（名字/创建时间/表数/是否当前库）、表列表（名字/列数/主键/创建/最后写入/行数）、schema（**直接复用 `TableSchema::serialize()` v1**） |
| `server/server.cpp` | 连接协程里处理 `kMeta`：读 Catalog（不占写槽）→ 回 `kMetaReply`；表不存在回 `ERROR` 帧 |
| `client/connection.cpp` | `RemoteConnection` 实现 `databases()` / `tables()` / `table_schema()`（`supports_metadata() == true`），REPL 的 `\l` / `\dt` / `\d` 因此对远程也生效 |

### 这一节踩到的坑（真 bug，只有"真起服务器"才抓得到）

`RemoteConnection` **不读 HELLO 帧**：连上之后服务器会先发 `HELLO`，而客户端
从第一条 `QUERY` 才开读 —— 结果第一条语句就把 `HELLO` 当成"不该出现的帧"，
统一报 `connection to server lost`（所有语句全失败）。

为什么之前没抓到：上一轮的 `RemoteClient` 用例在沙箱里被 `bind()` 拦下、
直接 skip 了，所以"编译过 + 别的用例绿"给了假绿灯。修法：`make_remote()` 里
先做一次 `handshake()`（读并校验 `HELLO` 的协议版本），失败就连接失败。

### 不依赖端口也能测客户端

新增 `tests/test_server/test_remote_client_fake_server.cpp`：用 `socketpair()`
造一条"已建立的连接"，一端跑**极简假服务器**（按协议应答 HELLO/OK/COLUMNS/ROW/
ERROR/META），另一端是真正的 `client::RemoteConnection` + 共用 REPL。这样受限
环境（`bind()` 被拒）也能验证客户端全部解析逻辑，并专门钉住上面那个 HELLO bug。
为此加了 `client::make_remote_from_fd(fd, label)`（测试/嵌入用的入口）。

当前：`test_server` 25 → **27 用例**（Protocol +1、RemoteClient +3、FakeServer +2），
`ctest` 12/12、干净重建 0 告警。

---

## 七、连接生命周期：空闲超时 / 优雅退出 / metrics（已落地）

### 1. 已写进代码（`server/server.*`、`server/main_server.cpp`）

| 能力 | 实现 |
|---|---|
| 空闲超时 | 每次等帧之前装一个**看门狗定时器**：不在事务里用 `idle_timeout_ms`，在事务里用 `idle_in_transaction_timeout_ms`（后者是防"事务挂着不动把 leveldb 旧版本钉住"）；用 generation 计数让旧定时器失效；到点只关**读方向**（`shutdown(fd, SHUT_RD)`），协程醒来后回一个 ERROR 帧说明原因（事务里先 `ROLLBACK`） |
| 优雅退出 | `SIGINT/SIGTERM` -> `Server::request_shutdown()` **只 write 一根 self-pipe**（async-signal-safe）-> 事件循环收到后：停 accept（unwatch）-> 通知所有连接收尾（`stopping` 标记 + generation++ + `shutdown(SHUT_RD)`）-> 等 `connections_ == 0`（上限 5s）-> 停 loop；`main` 之后 `store->close()`（**还有活跃快照时返回 Busy**，正好当"没退干净"的断言） |
| metrics | `connections_total / statements / errors / rows_sent / idle_timeouts / write_queue_rejected / parse_queue_rejected`，退出时打一行汇总 |
| 日志 | `server.log_level`（error/warn/info/debug）控制；连接 accept/close（debug）、超时与退出步骤（info/warn） |

### 2. 调试中抓到 + 已修的三个真 bug（用例现在全都开着）

`tests/test_server/test_lifecycle.cpp` 用 `socketpair + attach_connection()`
造连接（不占端口，受限沙箱里也能跑）。之前这里挂着的"写服务不回复 /
看门狗不触发"，根子是下面这些和写服务、看门狗本身都无关的 bug：

| # | Bug | 现象 | 修法 |
|---|-----|------|------|
| 1 | `attach_connection()` **没把 fd 设成非阻塞** | 一次 `read` 把事件循环线程堵死（定时器/别的连接全停） | 和 accept 路径一样先 `fcntl(O_NONBLOCK)` |
| 2 | 只 attach、没 `listen()` 时 `listen_fd_ == -1`，`accept_loop` 的 `WaitFd{fd<0}` 立刻 ready → 协程在 `while` 里**忙等不让出** | 整条事件循环被饿死：连 `SELECT` 都拿不到回复，**定时器也不走**（于是 `idle_timeouts` 一直是 0）——"写服务没回复"和"看门狗没触发"是同一个原因 | `run()` 里只在 `listen_fd_ >= 0` 时才 spawn accept 协程 |
| 3 | 往"对端已经消失"的 socket 上 `write()` 会送 **SIGPIPE**，默认处置直接杀掉进程 | 测试进程 `exit=141`（= SIGPIPE）；真实场景就是"客户端跑掉把服务器打死" | 新增 `common/socket_util.h`：写走 `send(MSG_NOSIGNAL)`，建连时再设 `SO_NOSIGPIPE` 兜底 |

第 3 条的两个细节（都实测过，免得以后怀疑）：

- 本机 macOS SDK **定义了 `MSG_NOSIGNAL` 且 `send()` 认它** —— 这条是
  socketpair 用例里真正生效的那条路；
- **`SO_NOSIGPIPE` 对 AF_UNIX socketpair 无效**（`setsockopt` 直接 `EINVAL`），
  只有 TCP 连接才吃这条 —— 所以真实 `sqldb-server`（TCP）两条都生效，而
  socketpair 用例只能靠 `MSG_NOSIGNAL`。

钉住这些行为的用例：空闲超时（含"服务器自己回 ERROR 帧"那条）/ 事务空闲超时 /
优雅退出 3 个原有用例 + `PeerAlreadyGoneDoesNotKillTheServer`
（服务器侧：对端先跑掉 → 服务器要活下来并收尾）；客户端侧另有
`FakeServer.WriteAfterServerIsGoneReportsConnectionLost`（对端没了要报
"connection to server lost"，而不是被信号带走）。把 `socket_write` 换回裸
`write()` 时，这两个用例立刻 `exit=141`，所以是真钉住了而不是"顺便通过"。

原来"下一步排查方向"里那两条猜测**都不成立**，不用再试：

- 看门狗不触发**不是**"`shutdown(SHUT_RD)` 叫不醒 `poll`"——AF_UNIX socketpair
  上实测能叫醒（`IdleTimeoutSendsErrorFrameOnItsOwn`：客户端一个字都不发，
  服务器自己回 ERROR 帧）；它和"写服务不回复"都是上面第 2 条（accept 协程忙等）
  造成的；
- 写服务路径本身没问题（同一套 `ServiceThread` 在真实监听路径下一直可用）。

夹具本身还修了一个隐藏 bug：`~Fixture` 与 `~RemoteConnection` 会关**同一个
fd**（双关），fd 号码被回收后可能关掉事件循环刚拿到的 fd（症状是"莫名其妙
的连接断开"）。现在所有权分清楚了：attach 之后归 `Server`，造出 connection
之后归 connection。

### 3. 验证

- `test_server` 30 → **33 用例**（ServerLifecycle +2、FakeServer +1），
  连跑 30 次稳定；`ctest` 12/12；
- `--clean-first` 干净重建 0 告警；
- **ThreadSanitizer（`-fsanitize=thread` 单独 build，5 次）0 竞态** —— 这批
  用例同时跑事件循环线程 + parse 线程 + 写线程 + 测试线程，是 TSan 最能出活的
  地方。空闲看门狗确实按预期触发（`idle_timeouts == 1`，事务里那条还会回滚）。

---

## 八、本轮改动记录（2026-09-13）：解封生命周期用例 + SIGPIPE 防护

第七节那三个用例原本是 `[todo]` 跳过的（打印提示后 return）。本轮把它们全部
打开、调通，并把第七节从"【进行中】/仍未解决"改写成"已落地"。这一节只记
**这一轮动了什么**，方便以后回溯；"为什么这么设计"看第七节。

### 1. 代码改动（1 个新文件 + 4 个文件）

| 文件 | 改动 |
|---|---|
| `common/socket_util.h`（新） | `socket_suppress_sigpipe(fd)`（建连时设 `SO_NOSIGPIPE`）+ `socket_write(fd, buf, n)`（`send(..., MSG_NOSIGNAL)`）。把"SIGPIPE 会按默认处置杀进程"这条平台差异收在一个地方，头文件注释里写清了两个平台的差别 |
| `server/server.cpp` | ① `run()` 只在 `listen_fd_ >= 0` 时才 `spawn(accept_loop())`；② `write_all()` 改走 `common::socket_write`；③ `attach_connection()` 与 accept 路径拿到 fd 后调 `common::socket_suppress_sigpipe(fd)` |
| `client/connection.cpp` | `RemoteConnection::send_all()` 改走 `common::socket_write`；`make_remote()` / `make_remote_from_fd()` 对 fd 设 `SO_NOSIGPIPE`（客户端的 socket 也是自己建的，同样不能被打死） |
| `tests/test_server/test_lifecycle.cpp` | 3 个用例去掉 `[todo]`；夹具拆成 `prepare()` / `attach_and_run()` / `connect_client()`（顺带修掉与 `~RemoteConnection` 的**双关**：现在 attach 后归 `Server`、握手后归 connection）；新增 `PeerAlreadyGoneDoesNotKillTheServer` 与 `IdleTimeoutSendsErrorFrameOnItsOwn` |
| `tests/test_server/test_remote_client_fake_server.cpp` | 新增 `WriteAfterServerIsGoneReportsConnectionLost` |

### 2. 三个 bug 的因果链（下次别再从错误方向查）

| # | Bug | 谁的症状 | 本轮状态 |
|---|---|---|---|
| 1 | `attach_connection()` 没设非阻塞 -> 一次 `read` 堵死事件循环线程 | 定时器/别的连接全停 | 上一轮已修，本轮有用例守着 |
| 2 | 只 attach、没 `listen()` 时 `listen_fd_ == -1`，`accept_loop` 的 `WaitFd{fd<0}` 立刻 ready -> 协程 `while` 里**忙等不让出** | **既是**"写服务语句拿不到回复"，**也是**"`idle_timeouts` 一直是 0"（定时器根本没机会跑） | 本轮修（`run()` 里加 `if (listen_fd_ >= 0)`） |
| 3 | 往"对端已经消失"的 socket 上 `write()` 送 SIGPIPE，默认处置杀进程 | 测试进程 `exit=141`；真实场景 = 一个跑掉的客户端把服务器打死 | 本轮修（`common/socket_util.h`） |

顺带修掉的夹具 bug：`~Fixture` 和 `~RemoteConnection` 会关**同一个 fd**。
fd 号码被回收后这一刀可能落在事件循环刚拿到的 fd 上，症状是"莫名其妙的连接
断开"——典型的"测试自己造的假故障"，很难查，所以写进这里。

### 3. 实测记录（都在这台机器上跑过，免得以后怀疑）

| 实验 | 结果 |
|---|---|
| 裸 `write()` 往"对端已关"的 AF_UNIX socketpair 写 | 进程被 SIGPIPE 杀掉（`exit=141`） |
| 同样场景改 `send(fd, buf, n, MSG_NOSIGNAL)` | 不杀进程，返回 `-1` + `EPIPE`；本机 macOS SDK **定义了 `MSG_NOSIGNAL` 且 `send()` 认它** |
| 同样场景只靠 `setsockopt(SO_NOSIGPIPE)` | **无效**：AF_UNIX socketpair 上 `setsockopt` 直接 `-1` + `errno=EINVAL`。所以这条只有 TCP 连接才吃得到，socketpair 用例只能靠 `MSG_NOSIGNAL` |
| 把 `socket_write` 换回裸 `write()` 再跑两个新用例 | 两个用例立刻 `exit=141` -> 说明用例**真钉住**了行为，不是"顺便通过" |
| 客户端一个字都不发，只看超时后收到什么 | 服务器自己回 `ERROR`（message 含 `idle`）-> `shutdown(SHUT_RD)` 确实能叫醒 `poll`，原猜测不成立 |

### 4. 验证

- `./build/run_tests/test_server`：**33 用例 / 191 断言 / 0 失败**，连跑 30 次稳定；
- `ctest --test-dir build`：**12/12**；
- 增量与 `--clean-first` 重建：0 告警；
- ThreadSanitizer（`-fsanitize=thread` 单独 build）重复跑：**0 竞态**；
- 空闲超时用例除 `idle_timeouts == 1` 外，事务里那条还确认**回滚生效**
  （另开会话读表 0 行），并确认收到的是**主动** ERROR 帧（客户端没有发任何东西）。

### 5. 环境限制（仍然存在，不影响上面的结论）

受限沙箱里 `bind()` 被拒，`ServerE2E`（6 条）与 `RemoteClient`（3 条真 TCP）
仍然打印 `[skip]`。要覆盖真实监听路径（TCP + `SO_NOSIGPIPE` 那条），
请在普通终端跑 `make server-test`。

---

## 九、本轮改动记录（2026-09-13）：通用配置包 + 读线程池 + Poller 抽象

三个相互独立、同一轮落地的改动。代码都已调通，这一节补上记录。

### 1. 通用配置包（`server/config.{h,cpp}` 重写）

`struct Config`（硬编码字段、解析时认字段）→ `class Config`（通用键值包）：

| 方面 | 做法 |
|---|---|
| 存储 | `map<section, map<key, sql::Value>>`（`std::less<>` 透明比较，`find(string_view)` 不造 string）；解析器**不认识任何具体字段**，只做语法 + 类型嗅探 |
| 三层防线 | ① 语法错（缺 `]`/`=`）→ `parse_config` 失败（带行号）；② 字段错（已知 section 里拼错的 key、类型不符、超范围/枚举外）→ `validate()` 失败（带字段名）——**内置默认值表同时充当 schema**（服务器认识哪些 key、各 key 什么类型）；③ 取值时类型不符 → 抛异常（validate 过了就不该发生） |
| 取值 | `CFG_INT(config_, server.max_connections)` 宏把字段名 `#field` 字符串化（"穷人反射"），字段名以代码标识符出现、和 `.ini` 键一一对应，不用手写字符串字面量 |
| 未知项 | 未知 **section** 合法（别的组件可以共用同一个配置文件）；已知 section 里的未知 **key** 被 `validate()` 拦下（多半是拼错了）——"拼错 key 不能静默用默认值"这条红线不变 |
| 启动路径 | `parse_config`/`load_config` → 命令行覆盖（`set()`，比如 `--listen`）→ `validate()` → `CFG_*` 取值；`host/port` 从结构体字段改成派生方法 `listen_host()/listen_port()` |

`test_config.cpp` 跟着重写（6 → 9 用例：默认值可用、分节解析、各类字段错
必须 validate 失败等）。所有取值点（`main_server` / `server` / 测试夹具）改走
`CFG_*`。

### 2. 读线程池（`server/server.{h,cpp}`、`server/service.h`）

M1 剩余项 `read_threads > 1` 落地，架构从"读池 = I/O 线程"变成真读池：

- `run()` 按 `execution.read_threads`（默认 1）起 N 条 `ServiceThread`
  （与 parse/write 服务同一套抽象），队列上限 `execution.read_queue_max`
  （新键，默认 1024）；
- 路由不变（`SELECT` 且不在事务 → 读），但"就地执行"改成**轮询投读池**
  （`read_next_` 原子计数取模，无锁）；队列满回 ERROR 帧并计
  `read_queue_rejected`（新 metric，退出汇总里多一项）；
- `Server::~Server` 与 `run()` 收尾都会停读池；
- `read_pool_size()` 暴露给测试。

用例 `ServerE2E.ReadPoolServesConcurrentReaders`：`read_threads=4`，
4 个线程各打 20 条纯 `SELECT`，全部要走读池且结果全对。注意这条用例里是
**多线程客户端**，不能用 `srvtest::Client`——它的 CHECK 会碰测试框架的
全局计数器，不是线程安全的；所以文件里新加了一组"裸"客户端助手
（`raw_connect`/`raw_row_count`，自己编解码帧）。

### 3. Poller 抽象（`server/poller.*` 新增，Loop 换后端）

决策 1 里"M3 换 kqueue/epoll"提前落地，学 libco 的第一点：**上层接口固定，
底层按平台条件编译换实现**：

| 后端 | 平台 | 文件 |
|---|---|---|
| kqueue | `__APPLE__` | `server/poller_kqueue.cpp` |
| epoll（LT） | `__linux__` | `server/poller_epoll.cpp` |
| poll(2) 兜底 | 任何平台 | `server/poller_poll.cpp` |

- 三个 `.cpp` **全平台都编译**（文件内 `#if defined(...)` 自己守），
  `Poller::create()` 按平台挑实现，平台后端创建失败也退回 poll；
- 语义统一成 poll(2) 的词汇：**水平触发** + 事件位
  `POLLIN/POLLOUT/POLLERR/POLLHUP/POLLNVAL`，上层（Loop/WaitFd）只看到
  这一套。epoll 特意**不开 EPOLLET**：边缘触发会改变"协程恢复后重试
  系统调用"的上层约定；
- `Loop` 的改动：wakeup 管道**常驻注册**、不随 `watches_` 摘挂；
  `watch/unwatch` 同步进 poller；事件派发仍是"先摘出回调再调用"（回调里
  可能重新 watch/关 fd）；`POLLNVAL` 的 fd 顺手摘掉，别反复报。

用例 `test_poller.cpp` 2 条（跑在当前平台后端上）：读就绪/水平触发语义/
unwatch 后不再报；写就绪/对端关闭要报（`POLLIN|POLLHUP` 任一，各后端词汇
略有差异）。

### 4. 顺带的夹具/构建改动

| 文件 | 改动 |
|---|---|
| `tests/test_server/storage_helper.h` | `RunningServer` 加 `ConfigTweak`（构造期调配置，比如把 `read_threads` 调大）与 `running()` 访问器；配置写入改走 `set()` |
| `CMakeLists.txt` | `poller_poll/kqueue/epoll.cpp` 进 `sql_server` |

### 5. 验证

- `./build/run_tests/test_server`：33 → **39 用例 / 231 断言 / 0 失败**
  （Config 6→9 重写、Poller +2、ServerE2E +1），连跑 30 次稳定；
- `ctest --test-dir build`：**12/12**；
- `--clean-first` 干净重建：**0 告警**；
- **ThreadSanitizer 0 竞态**（`-fsanitize=thread` 单独 build，5 次）——读池
  给 TSan 新增了"事件循环线程 × N 条读线程"的交叉面，正是该它出活的地方。

### 6. 环境限制（新踩到一条）

- `bind()` 沙箱限制仍在：`ReadPoolServesConcurrentReaders` 是真 TCP 用例，
  沙箱里同样 `[skip]`；普通终端跑 `make server-test` 才是全量；
- TSan 构建要用 **MacPorts clang-23 工具链**（跟 `CMakePresets.json` 的
  `llvm-debug` 同一套，加 `-fsanitize=thread` 即可）；直接 `cmake -B
  build-tsan` 会用 Apple Clang，它的 libc++ 太旧，连 C++23 的
  `construct_at` 都编不过。
