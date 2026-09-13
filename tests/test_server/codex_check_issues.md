# test_server：协程服务器模块的改动记录

`tests/test_server/` 是服务器（`server/` + `client/`）的测试目录。这份文档记录
服务器相关的**设计决策、P0 发现与踩坑**；M1 的测试用例落地后，用例清单也追加
在这里（测试代码本身进 `tests/test_server/`，跑法见 `Makefile` 的 `server-test`）。

现状：**代码还没开始写**，先做了上服务器前的 P0 体检（第一节）。

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
| 1 | 协程自研：C++20 `<coroutine>` + `poll`（M3 换 kqueue/epoll）+ 跨线程唤醒 pipe；**不用 ucontext**（arm64 macOS 不可用） |
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

1. `client/` 抽取：`SqlConnection`（本地包 `Session` / 远程走协议）+ 共用 REPL +
   `sqldb-client`（本地 `sqldb` 行为不变）。
2. 连接超时（idle / idle-in-transaction）、`max_result_rows` 已有、`statement_timeout`
   （要等 M3 的协作检查点才能真正打断）、优雅退出（现在 SIGINT 只停 loop）。
3. 日志与 metrics（连接数/语句数/队列深度）；`read_threads > 1`（现在读池就是 I/O 线程）。
