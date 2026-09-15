# 补充材料：C++23 的协程原理和库开发模式

> 本文是 `common/svrkit` 的配套教学材料。`design.md` 回答"svrkit 为什么这样设计、
> 源码怎么逐行验证"；本文回答"从 C++ 语言本身出发，协程到底是什么，以及一个
> 协程框架是怎么一步一步搭出来的"。读法：配合 `common/svrkit/task.h`、`loop.cpp`、
> `service.cpp`、`tcp_server.cpp` 一起看，代码全部来自本仓库。

## 目录

1. [协程的原语和结构](#1-协程的原语和结构)
2. [基于原语开发什么组件](#2-基于原语开发什么组件)
3. [协程组件在多线程程序中的调度和队列](#3-协程组件在多线程程序中的调度和队列)
4. [如何完成一个最小的基于协程的异步 echo server](#4-如何完成一个最小的基于协程的异步-echo-server)

## 第 0 步：先把"C++23 协程"这个说法说清楚

- 协程作为**语言特性**在 C++20 就已经定型：`co_await` / `co_return` / `co_yield`、
  `std::coroutine_handle`、`promise_type`。
- C++23 在标准库里新增了第一个协程类型 `std::generator<T>`（头文件 `<generator>`），
  它解决的是"同步惰性序列"（生成器），和本文讨论的"事件驱动 I/O 协程"是两种用法。
- 本项目以 `cxx_std_23` 编译（见根目录 `CMakeLists.txt`），但 svrkit 用到的全部是
  C++20 核心原语，标准库只用到 `<coroutine>`。
- 所以下文说"C++23 协程"时，指的是"在 C++23 编译环境下使用 C++20 引入的协程原语"，
  这已经是稳定、可移植、被主流编译器完整支持的能力。

## 1. 协程的原语和结构

### 1.1 从问题出发：为什么需要协程

**阻塞 I/O 的线程模型**：每连接一线程，N 个连接要 N 个线程。每个线程大部分时间都
sleep 在等待内核事件上，还要为每个线程维护独立的栈（通常 8MB 虚拟内存）。N 到一万
时，线程本身就成了瓶颈。

**非阻塞 I/O 的回调模型**（select/epoll + 回调）：能撑住高并发，但"读 -> 处理 ->
写"的逻辑被打散成多个回调，状态存在闭包里，代码不可读、不可调。

**协程是第三条路**：把函数写成顺序的 `read -> process -> write`，但让它在"等 I/O"
的地方**挂起（suspend）**，把线程让给别人；等事件就绪了再由事件循环**恢复
（resume）**。于是**一个线程可以同时跑成千上万个"挂起中"的协程**，每个协程只占
一份内存，不占线程。

### 1.2 协程 = 可挂起的函数

普通函数：调用 -> 执行 -> 返回。调用者的执行不被中断，函数栈帧随调用结束销毁。

协程函数：执行到 `co_await` / `co_return` / `co_yield` 时可以挂起，把控制权交回给
调用者或某个调度源；之后由 `coroutine_handle::resume()` 恢复，从**挂起点**继续执行。

关键点：**挂起期间协程不占线程**。它只占一份动态分配的内存——协程帧
（coroutine frame）——里面记录"函数执行到哪、局部变量是什么、挂起的 awaitable 临时
对象在哪"。

C++ 的协程是 **stackless** 的：没有自己的调用栈，不能像线程那样随意"换栈"。挂起/
恢复只能发生在 `co_await` 表达式处。这既是限制（递归、大量跨挂起点的自动变量要
小心），也是它廉价的原因——一个协程的成本约等于一份状态机内存。

### 1.3 协程帧（coroutine frame）：编译器替你管理的状态机

写一个协程函数：

```cpp
Task handler(TcpConnection &conn) {
  std::string buf;
  ssize_t n = 0;
  co_await conn.read_some(buf, n);   // 挂起点 A
  bool ok = false;
  co_await conn.write_all(buf, ok);  // 挂起点 B
}
```

编译器会把它改造成一个**状态机**，并生成一份"协程帧"的内存布局：

```text
协程帧（coroutine frame，通常堆上分配）
├── promise_type 对象            ← 协程与调用者之间的共享状态
├── 参数副本                     ← 按值传入的参数
├── 跨挂起点存活的局部变量        ← buf、n、ok 等
├── 当前挂起点的编号              ← 状态机 PC："下次从 A 还是 B 继续"
└── 挂起期间仍活着的 awaitable    ← co_await 表达式的临时对象
```

帧的持有者就是返回给调用者的对象（比如 `Task`），它内部包着一个
`std::coroutine_handle`，指向这个帧。

> **调用一个协程函数，只做两件事：分配帧、返回句柄。函数体不一定立刻执行。**
> 是否立刻执行由 `initial_suspend()` 决定（见 1.4）。这是理解 svrkit 里
> `Loop::spawn()` 与嵌套 `co_await` 行为的前提。

### 1.4 三个语言原语：handle / promise / co_await

**① `std::coroutine_handle<>`**：不透明句柄，指向协程帧，是"调度"协程的唯一通道。

```cpp
handle.resume();     // 从挂起点继续执行（同步！调用它的线程会一路跑到再次挂起）
handle.destroy();    // 销毁帧（不销毁会泄漏）
handle.done();       // 是否已执行到 final_suspend
handle.promise();    // 访问 promise_type 对象
```

`resume` 是同步的：谁调用 `resume`，就在谁的线程上执行这段协程，直到它再次挂起或
结束。**因此"在哪个线程恢复协程"完全由调度源决定**——这就是多线程模型的核心。

**② `promise_type`**：定义在协程返回类型里，是协程和外部世界的共享状态。编译器
固定调用以下方法：

| 方法 | 调用时机 | 作用 |
|---|---|---|
| `get_return_object()` | 帧创建后 | 构造返回给调用者的对象（如 `Task`） |
| `initial_suspend()` | 函数体执行前 | 决定"调用时就开跑"还是"先挂起等 resume" |
| `final_suspend()` | 函数体结束 | 决定结束后的处理（转交 continuation / 安排销毁） |
| `yield_value(v)` | `co_yield v` | 生成一个值并挂起 |
| `return_void()` / `return_value(v)` | `co_return` | 写回结果 |
| `unhandled_exception()` | 函数体抛异常 | 保存 `exception_ptr`，供恢复方重新抛 |

**③ `co_await expr`**：唯一的挂起点。编译器对它做如下展开：

```text
1. 拿到 awaitable 的 awaiter（它自身，或 operator co_await 的结果）
2. awaiter.await_ready() ?
     true  → 不挂起，直接 await_resume()，继续执行
     false → awaiter.await_suspend(handle)：
              返回 void              → 挂起，控制权交回"resume 我的人"
              返回 bool (false)      → 不挂起，立即继续
              返回 bool (true)       → 挂起
              返回 coroutine_handle  → 挂起并对称转移到另一个协程
3. 之后某个线程 resume 我，执行 await_resume()，取到结果（或抛异常）
```

所以一个 **awaiter** 只需要三个方法，语义极其清晰：

| 方法 | 执行时机/线程 | 职责 |
|---|---|---|
| `await_ready()` | 挂起前，当前线程 | 快速路径：条件已满足就不挂起 |
| `await_suspend(handle)` | 挂起前，当前线程 | **把 handle 交给某个调度源**；返回后协程挂起 |
| `await_resume()` | 恢复后，恢复线程 | 返回结果，或抛出保存的异常 |

### 1.5 最小可运行示例：手写一个 Generator

不依赖任何库，只用 `<coroutine>`，把上面三个原语全部走一遍：

```cpp
#include <coroutine>
#include <cstdio>

struct Generator {
  struct promise_type {
    int value = 0;
    Generator get_return_object() {
      return Generator{
          std::coroutine_handle<promise_type>::from_promise(*this)};
    }
    std::suspend_always initial_suspend() noexcept { return {}; }
    std::suspend_always final_suspend() noexcept { return {}; }
    std::suspend_always yield_value(int v) noexcept { value = v; return {}; }
    void return_void() noexcept {}
    void unhandled_exception() noexcept { std::terminate(); }
  };

  explicit Generator(std::coroutine_handle<promise_type> h) : h_(h) {}
  Generator(const Generator &) = delete;   // 帧只能有一个 owner
  Generator &operator=(const Generator &) = delete;
  Generator(Generator &&o) noexcept : h_(o.h_) { o.h_ = {}; }
  ~Generator() { if (h_) h_.destroy(); }

  bool next() {
    if (!h_ || h_.done()) return false;
    h_.resume();
    return !h_.done();
  }
  int value() const { return h_.promise().value; }

private:
  std::coroutine_handle<promise_type> h_;
};

Generator counter() {
  for (int i = 0; i < 3; ++i) {
    co_yield i;      // 等价于 co_await promise.yield_value(i)
  }
}

int main() {
  Generator g = counter();   // 只建帧，不执行（initial_suspend 挂起）
  while (g.next()) {         // 每次 resume 跑一段
    std::printf("got %d\n", g.value());
  }
}
```

输出：

```text
got 0
got 1
got 2
```

这里能直观看到：`counter()` 调用不执行函数体；每 `co_yield` 挂起一次；`next()` 负责
`resume`；`promise_type` 是数据通道（`value`）；`~Generator` 负责 `destroy` 帧。

### 1.6 svrkit 的 Task 就是这一套原语的最小封装

对照 `common/svrkit/task.h`，逐字段看：

```cpp
class Task {
public:
  struct promise_type {
    std::coroutine_handle<> continuation{};   // 本协程结束后 resume 谁
    std::exception_ptr error;                 // 异常通道
    Task get_return_object();
    std::suspend_always initial_suspend() noexcept { return {}; }  // 懒启动
    struct FinalAwaiter {                     // 结束挂起点
      bool await_ready() noexcept { return false; }
      void await_suspend(std::coroutine_handle<promise_type> h) noexcept;
      void await_resume() noexcept {}
    };
    FinalAwaiter final_suspend() noexcept { return {}; }
    void return_void() {}
    void unhandled_exception() { error = std::current_exception(); }
  };
  // ... move-only，持有 handle_，析构时 destroy()
};
```

把 svrkit 的设计决策逐条对应回原语：

1. **只有 `Task<void>`**：结果一律通过**引用参数**写回（`read_some(buf, n)`）。因为
   调用方也是协程、帧在挂起期间一直活着，引用稳定。省掉一整套 `Task<T>` 模板、
   嵌套返回值和异常传播的样板。
2. **`initial_suspend` = `suspend_always`（懒启动）**：调用 `handler(...)` 只建帧；
   要跑必须 `handle.resume()`。这正是 `Loop::spawn()` 的职责。
3. **`continuation` 字段**：子协程被 `co_await` 时，`Task::await_suspend(caller)`
   把 caller 记进 `continuation`，然后立即 `resume()` 子协程：

   ```cpp
   void Task::await_suspend(std::coroutine_handle<> caller) noexcept {
     handle_.promise().continuation = caller;
     handle_.resume();   // 子协程跑到它的第一个真实挂起点才让出
   }
   ```

4. **`FinalAwaiter`（final_suspend）**：函数体跑完后，如果还有 continuation，就
   **对称转移**回去（同一线程直接 resume，不绕事件循环）；没有 continuation 说明是
   顶层协程，`post` 一个 `destroy()` 给 Loop，由 Loop 销毁帧。
5. **`error` 字段 + `await_resume()` rethrow**：子协程抛的异常存进 `exception_ptr`，
   父协程在 `co_await` 恢复点重新抛——所以 `TcpServer::run_connection()` 能用
   try/catch 包住 `co_await handler_(connection)`。

帧生命周期小结：

```text
Loop::spawn(task)
  task.release() → Loop 持 handle
  Loop post(handle.resume())
    协程函数体开始跑（initial_suspend 已过）
    |-- WaitFd / SleepFor / SubmitToService：挂起，帧继续活着
    |-- co_return：进入 final_suspend
  FinalAwaiter：有 continuation → resume 父协程（帧由父协程的
                co_await 表达式结束时析构）
               没有 continuation → Loop post(handle.destroy())
```

## 2. 基于原语开发什么组件

语言只给了"帧 + 句柄 + 三个方法"的乐高积木。剩下的事情——**谁负责 resume、什么时候
resume**——全要靠库。svrkit 就是在这些原语上搭出的一层薄框架。组件清单：

| 组件 | 基于的原语 | 职责 | 代码位置 |
|---|---|---|---|
| `WaitFd` | awaiter 协议 | 把 fd 就绪事件翻译成挂起/恢复 | task.h |
| `SleepFor` | awaiter 协议 | 定时挂起 | task.h |
| `Loop` | coroutine_handle 的调度 | 每线程事件循环：Poller + 定时器 + 跨线程投递 | loop.h/cpp |
| `ServiceThread` / `SubmitToService` | awaiter 协议 + 有界队列 | 把重活投到专用线程，完成后回原 Loop 恢复 | service.h/cpp |
| `TcpConnection` | Task + WaitFd | 协程化的非阻塞读写 | tcp_server.h/cpp |
| `TcpServer` | Task + Loop + spawn | listen/accept、连接上限、连接所有权、优雅退出 | tcp_server.h/cpp |

### 2.1 分层图

```text
应用 handler（echo / SQL / Raft）
        │  Task，顺序代码，内含 co_await
        ▼
Task + WaitFd / SleepFor / SubmitToService
        │  await_suspend 把 handle 交给调度源
        ▼
Loop（每线程一个）
        │  Poller 事件 / timer 到期 / 跨线程 post
        ▼
Poller（kqueue / epoll / poll，统一成 poll 词汇、水平触发）
        │  fd readiness
        ▼
TcpServer / TcpConnection
```

### 2.2 WaitFd：把 fd 事件变成挂起点

```cpp
struct WaitFd {
  int fd = -1;
  short events = 0;                        // POLLIN / POLLOUT
  bool await_ready() const noexcept { return fd < 0; }  // 快速路径
  void await_suspend(std::coroutine_handle<> handle);   // 交给 Loop
  short await_resume() const;              // 返回 poll 事件位
  mutable short revents_ = 0;              // 事件到达时由 Loop 回调写入
};
```

实现（loop.cpp）：

```cpp
void WaitFd::await_suspend(std::coroutine_handle<> handle) {
  Loop *loop = current_loop();
  if (loop == nullptr) return;             // 不在 Loop 里跑 = 误用，直接恢复
  WaitFd *self = this;                     // 临时量在挂起期间一直活着
  loop->watch(fd, events, [self, handle](short revents) {
    self->revents_ = revents;
    handle.resume();                       // 事件来了：恢复协程
  });
}
```

三个方法各司其职：`await_ready` 做快速判断（fd 无效直接继续，不挂起）；
`await_suspend` 把 handle 注册进 Loop 的 fd 回调表，然后协程挂起；事件到达后回调写
`revents_` 并 `resume`，`await_resume` 把事件位交给调用方。

配合非阻塞 I/O 的完整模式（`TcpConnection::read_some` 的循环）：

```text
read(fd)
  ├─ got > 0      → 返回
  ├─ got == 0     → 对端关闭
  ├─ EAGAIN       → co_await WaitFd{fd, POLLIN}，恢复后 continue 重试
  ├─ EINTR        → continue
  └─ 其他错误     → 返回 -1
```

"非阻塞 + 水平触发 + 恢复后重试系统调用"是一个不会丢事件、也不会忙等的组合。Poller
特意不用 `EPOLLET`（边缘触发），保证恢复后重新 read 一定还能拿到数据或 EAGAIN。

### 2.3 SleepFor：定时器挂起点

```cpp
void SleepFor::await_suspend(std::coroutine_handle<> handle) const {
  loop->add_timer(ms, [handle] { handle.resume(); });
}
```

它不碰 fd，只是把 handle 放进 Loop 的定时器表。到期后 `Loop::run_due_timers()` 先把
到期回调摘出来再执行，因此回调里可以安全地新增定时器。

### 2.4 Loop：每线程一个"调度源"

协程库的心脏是**调度源**：一个能"在未来的某个时刻 resume 某个 handle"的实体。svrkit
的选择是一个 `Loop` 对应一个线程、归属且只归属跑 `run()` 的那个线程：

```cpp
class Loop {
public:
  void run();                          // 阻塞当前线程的事件循环
  void post(std::function<void()>);    // 线程安全：跨线程投递
  void spawn(Task);                    // 线程安全：启动顶层协程
  void watch(int fd, short events, std::function<void(short)>);
  void unwatch(int fd);
  void add_timer(int64_t delay_ms, std::function<void()>);
  // watch/unwatch/add_timer 只能在 Loop 线程调用
private:
  std::unique_ptr<net::Poller> poller_;
  std::mutex mutex_;                   // 保护 pending_
  std::deque<std::function<void()>> pending_;
  int wakeup_read_ = -1, wakeup_write_ = -1;   // self-pipe，唤醒 poll
  std::unordered_map<int, Watch> watches_;
  std::vector<std::pair<int64_t, std::function<void()>>> timers_;
};
```

`thread_local Loop *g_loop`（`current_loop()`）记录"当前线程跑的是哪个 Loop"。所有
awaitable 的 `await_suspend` 都通过 `current_loop()` 找到自己所属的 Loop、把 handle
交给它——这保证了**协程不会跨 Loop 迁移**。

### 2.5 ServiceThread / SubmitToService：把重活离线

事件循环线程最忌讳做重活（协议解析、执行 SQL、apply 日志），因为那会让所有连接一起
卡住。解法是"离线执行 + 完成后回跳"。协程里这样用（server.cpp 的真实写法）：

```cpp
common::svrkit::SubmitToService submit;
submit.service = &write_service_;
submit.work = [&session, &parsed, &result, this] {
  result = execute_on_this_node(session, *parsed, *store_);   // 跑在 service 线程
};
co_await submit;
if (!submit.submitted) {
  // 队列满：这是背压信号，应用层转成协议错误（回一个 "server busy" 帧）
}
```

`SubmitToService::await_suspend`：

```cpp
void await_suspend(std::coroutine_handle<> handle) {
  submitted = service->submit(std::move(work), current_loop(), handle);
  if (!submitted) {
    handle.resume();   // 没排上：立刻恢复，让调用方看到 submitted == false
  }
}
```

worker 侧（`ServiceThread::run`，service.cpp）：

```cpp
work();                                       // 重活跑在 service 线程
caller_loop->post([handle] { handle.resume(); });   // 回到发起 Loop 恢复
```

**恢复一定发生在原 Loop，不在 worker 线程**——这是整个多线程模型不出错的关键，
第 3 步会详细论证。

### 2.6 TcpConnection：协程化 I/O

```cpp
Task read_some(std::string &buffer, ssize_t &nread) const;  // 返回 Task<void>
Task write_all(std::string data, bool &ok) const;
```

每个连接的读写语义上就是同步代码，内部对 EAGAIN 用 `co_await WaitFd` 挂起。应用
handler 可以顺序地写：

```cpp
std::string input;
ssize_t nread = 0;
co_await conn->read_some(input, nread);
if (nread > 0) {
  bool ok = false;
  co_await conn->write_all(input, ok);
}
```

### 2.7 TcpServer：连接生命周期

- `listen(host, port)`：建 listen socket；
- `run()`：watch 信号 self-pipe（优雅退出入口），若 listen socket 有效则
  `loop_.spawn(accept_loop())`，然后进 `Loop::run()`；
- `accept_loop()` 是协程：`co_await WaitFd{listen_fd, POLLIN}`，可读后**循环** accept
  到 EAGAIN（一个事件带走 backlog 里的多个连接）；每个新连接做连接数上限检查（超限
  直接 close）、设非阻塞、抑制 SIGPIPE、`loop_.spawn(run_connection(conn))`；
- `run_connection`：`try { co_await handler_(conn); } catch (...) { log; }`，返回后
  `close_connection`（取回 fd、close、从连接表删除、计数减一）；
- `request_shutdown()` 只写一个字节到 self-pipe（async-signal-safe，可从信号处理函数
  调用）；Loop 醒来后停止 accept、对所有连接 `mark_stopping()` + `shutdown_read()`，
  每 20ms 检查连接数，清零或超过 grace 期限后 `loop_.stop()`——这就是优雅退出。

### 2.8 开发协程组件的三条纪律

1. **一个协程只属于一个 Loop**：跨线程只允许 `post()` 一个"继续执行"的动作，
   绝不允许别的线程直接 `resume` 你的 handle。
2. **Loop 的私有 API 只在 Loop 线程调用**：`watch/unwatch/add_timer` 不是线程安全
   的；跨线程要 `post()` 回来再调。
3. **重活必须离线，且队列必须有界**：队列满要**可见地失败**（`submit() == false`），
   让应用层实现背压，而不是无限堆任务。

## 3. 协程组件在多线程程序中的调度和队列

### 3.1 线程模型全景

```text
                  ┌─────────────────────────────────────────┐
 main / 其他线程  │  request_shutdown() / post() / submit()  │  线程安全入口
                  └──────────────────┬──────────────────────┘
                                     ▼
  ┌──────────────────────── Loop 线程（I/O）────────────────────────┐
  │  drain pending_ ── run due timers ── poller.wait ── dispatch    │
  │  唤醒来源：fd 事件 / 定时器到期 / 跨线程 post(self-pipe)          │
  │  上面挂着 N 个连接协程（每个都在某个挂起点等待）                   │
  └──────────────▲──────────────────┬──────────────────────────────┘
                 │ post(resume)     │ submit(work, caller_loop, handle)
                 │                  ▼
       ┌─────────┴──────────┐   ┌──────────────────────────────┐
       │ ServiceThread (1..N)│   │  有界队列 queue_ + mutex + cv │
       │  parse / write /    │   │  work 离开 Loop 线程执行，    │
       │  read pool           │   │  完成后 caller_loop.post      │
       └─────────────────────┘   └──────────────────────────────┘
```

所有让协程"继续跑"的路径，最终都收敛到 **Loop 线程的 `resume`**。这是模型简单的根本
原因：**跨线程通信全部走队列 + 事件循环，不存在两线程同时执行同一段协程**。

### 3.2 Loop 的调度循环（loop.cpp 的 `run()`）

```text
while (!stop_) {
  1) 加锁 swap 出 pending_，逐个执行（跨线程投递的动作）
  2) run_due_timers()：摘出所有到期的定时器回调，逐个执行
  3) 计算 poll 超时 = 最近一个定时器的剩余时间（没有则 1s 兜底唤醒）
  4) poller_->wait(timeout)：阻塞等 fd 事件
  5) 对每个就绪 fd：先 move 出回调并 unwatch(fd)，再调用回调
}
```

第 5 步的"**先摘回调再调用**"是铁律：回调里可能重新 `watch` 同一个 fd、关闭 fd、或
启动别的逻辑。如果边遍历 `watches_` 边改它，unordered_map 的迭代器就失效了。

### 3.3 跨线程投递：post + self-pipe

`post()` 是 Loop 暴露给其他线程的唯一入口：

```cpp
void Loop::post(std::function<void()> action) {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    pending_.push_back(std::move(action));
  }
  wakeup();   // 往 self-pipe 写一个字节，唤醒正在 poll 的线程
}
```

self-pipe 两端非阻塞（关键：drain 到读不到为止时最后一次 read 要返回 EAGAIN 而不是
阻塞，否则整个事件循环会被挂死）。Loop 在 poll 醒来后 `drain_wakeup()` 把管道读空。
这样"另一个线程想让我干活"这件事，被翻译成一个 fd 事件，**统一走事件循环**。
`stop()` 和 `request_shutdown()` 也是同一个机制。

### 3.4 三种唤醒来源

| 唤醒来源 | 谁触发 | 对应的 awaitable | 恢复方式 |
|---|---|---|---|
| fd 就绪 | Poller（kqueue / epoll / poll） | `WaitFd` | 回调写 `revents_` 后 `handle.resume()` |
| 定时器到期 | `run_due_timers()` | `SleepFor` | 回调 `handle.resume()` |
| 跨线程投递 | 其他线程 `post()` / ServiceThread 完成 | `SubmitToService` | 动作里 `handle.resume()` |

### 3.5 队列：两种队列，两种约束

**Loop 的 `pending_`（无界 deque，mutex 保护）**：装的只是"继续执行"的记号，动作本身
很轻（通常就是一次 `resume`）。无界是安全的，因为投递速度天然受限于"真有一个线程在
做事"。

**ServiceThread 的 `queue_`（有界 deque，mutex + condition_variable）**：装的是
**重活**，可能很慢。无界会让内存被积压的任务吃掉，所以必须：

```cpp
if (stopping_ || queue_.size() >= queue_max_) {
  return false;              // 队列满：submit 失败
}
```

失败 → `SubmitToService::submitted == false` → 协程立即恢复 → 应用层把它转成协议错误
（server.cpp 里就是回一个 `server busy` 帧）。这就是**背压**：队列不吸收压力，而是把
压力显式还给上游。注意队列满时 `await_suspend` 里 `handle.resume()` 是**立即恢复**，
协程并没有真正挂起过，所以不会丢事件、也不会死锁。

### 3.6 一个请求跨线程的完整时序

```text
Loop 线程
  TcpConnection::read_some
    read(fd) → EAGAIN
    WaitFd{fd, POLLIN}.await_suspend
      Loop.watch(fd, callback)
    协程 A 挂起
  Loop 继续处理其他 fd / timer

客户端写入数据
  Poller 返回 fd readable
  Loop: move 出回调并 unwatch(fd)
    回调: A.revents_ = POLLIN; A.resume()
  read_some: read(fd) → n bytes，co_return

handler
  co_await SubmitToService
    协程 A 挂起；work 进 ServiceThread 队列

ServiceThread（worker 线程）
  执行 work（解析 / 执行 SQL）
  caller_loop.post([A]{ A.resume(); })

Loop 线程
  drain pending_ → 协程 A resume
  write response（可能再挂起在 WaitFd{POLLOUT} 上）
```

整个过程中，线程没有阻塞在任何一个连接上；每个等待点都显式交还了控制权。

### 3.7 为什么不会出数据竞争：核心论证

1. **一个协程只被创建它的 Loop 恢复**：所有 awaitable 的 `await_suspend` 都经
   `current_loop()` 注册，`ServiceThread` 完成时也是 `caller_loop->post` 回来。
2. **协程挂起期间，没有任何线程执行它的帧**。因此 work 通过引用写协程帧上的局部变量
   是安全的——那个变量此刻没人碰。
3. **恢复 = 同一时刻只有一个线程跑这个协程帧**。跨线程边界只发生在两处：
   - "注册 handle"（awaiter 构造 / 挂起点），由当前 Loop 线程串行完成；
   - "post 恢复"（把 resume 动作原子地塞进 Loop 队列），由 mutex + 事件循环串行化。

要维护的正确性不变量（与 design.md 第 11 节一一对应）：

- I1 一个协程只属于一个 Loop；
- I2 Loop 私有 API 只在 Loop 线程调用；
- I3 同一 fd 同一时刻只有一个 watcher（连接协程独占 fd）；
- I4 事件派发前先摘回调；
- I5 非阻塞 + 水平触发 + 恢复后重试；
- I6 Task frame 生命周期覆盖挂起（临时 awaitable 在挂起期间一直活着）；
- I7 Service work 只碰协程帧局部结果或线程独享状态；
- I8 队列必须有界。

## 4. 如何完成一个最小的基于协程的异步 echo server

现在把前面所有组件组装起来：一个基于 svrkit 的 echo server，几十行，单线程同时服务
大量连接，每个连接一个协程。

### 4.1 完整代码

```cpp
// echo_server.cpp：最小协程异步 echo server（svrkit 版）
#include <cstdio>
#include <memory>
#include <string>
#include <sys/types.h>

#include "common/svrkit/task.h"
#include "common/svrkit/tcp_server.h"

using common::svrkit::TcpConnection;
using common::svrkit::TcpServer;
using common::svrkit::TcpServerOptions;
using common::svrkit::Task;

Task echo_handler(std::shared_ptr<TcpConnection> conn) {
  while (true) {
    std::string input;
    ssize_t nread = 0;
    co_await conn->read_some(input, nread);        // 等数据（挂起，不占线程）
    if (nread <= 0) {
      break;                                        // 0 = 对端关闭，-1 = 出错
    }
    bool ok = false;
    co_await conn->write_all(std::move(input), ok); // 原样写回
    if (!ok) {
      break;                                        // 写失败：放弃这条连接
    }
  }
  // 返回后 TcpServer 统一 close(fd) 并注销连接
}

int main() {
  TcpServerOptions options;
  options.max_connections = 256;
  options.logger = [](const char *level, const std::string &msg) {
    std::fprintf(stderr, "[%s] %s\n", level, msg.c_str());
  };

  TcpServer server(options, echo_handler);
  auto ok = server.listen("127.0.0.1", 9000);
  if (!ok.has_value()) {
    std::fprintf(stderr, "listen failed: %s\n", ok.error().c_str());
    return 1;
  }
  std::fprintf(stderr, "echo server on 127.0.0.1:9000\n");
  server.run();                                     // 阻塞当前线程跑事件循环
  return 0;
}
```

> 这段代码在本仓库以 `clang++ -std=c++23` 实测编译通过，并用 python 客户端回显验证
> 通过（`hello coroutine\nsecond line\n` 原样返回，日志出现
> `connection accepted` → `connection closed`）。

### 4.2 编译

项目里 svrkit 已经是现成的 CMake 目标：`svrkit`（链接 `common_net` + `Threads::Threads`，
编译特性 `cxx_std_23`）。临时验证可以复用 build 产物直接编：

```bash
# 在项目根目录
/opt/local/bin/clang++-mp-23 -I. -stdlib=libc++ -std=c++23 \
    echo_server.cpp build/libsvrkit.a build/libcommon_net.a -pthread \
    -o echo_server
```

正式接入时在 CMakeLists.txt 里加：

```cmake
add_executable(echo_server echo_server.cpp)
target_include_directories(echo_server PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(echo_server PRIVATE svrkit)
```

### 4.3 运行与联调

```bash
./echo_server
# 另一个终端，用 nc 发送两行：
printf 'hello coroutine\nsecond line\n' | nc 127.0.0.1 9000
# 立即原样回显：
# hello coroutine
# second line
```

服务端日志：

```text
echo server on 127.0.0.1:9000
[debug] connection accepted
[debug] connection closed
```

多开几个 nc 同时连，全部能同时回显——**只有一个线程**，但每个连接都有独立的协程
状态，互不干扰。

### 4.4 逐行追踪：一条数据走了哪条路

1. `main()`：构造 `TcpServer`（内部创建一个名为 `tcp-server-io` 的 Loop），
   `listen()` 建监听 socket。
2. `server.run()`：
   - watch 信号 self-pipe（优雅退出入口）；
   - `loop_.spawn(accept_loop())`——注意 `accept_loop()` 此刻**没有执行**，只是建了帧
     放进 post 队列（`initial_suspend` 懒启动）；
   - 进入 `Loop::run()`：drain 队列时执行 accept 协程的第一次 resume。
3. `accept_loop` 第一次跑：`co_await WaitFd{listen_fd, POLLIN}` 挂起。事件循环开始轮询。
4. 客户端 connect：Poller 报 listen fd 可读。Loop 摘出回调、resume accept 协程 →
   循环 accept（**一个事件可能带走 backlog 里的多个连接**）→ 每个连接检查连接上限 →
   `attach_connection` → `loop_.spawn(run_connection(conn))`。
5. `run_connection` resume 后 `co_await echo_handler(conn)`：因为 `Task` 的
   `initial_suspend` 是挂起，`Task::await_suspend(caller)` 会**立即 resume 子协程**，
   `echo_handler` 开始跑。
6. `echo_handler` 调 `read_some`：`read(fd)` 返回 EAGAIN（客户端还没发数据）→
   `co_await WaitFd{fd, POLLIN}` 挂起。此时这条连接的状态是：**一个协程帧 + watches_
   里一条 fd 记录**，零线程占用。
7. 客户端发 `hello`：Poller 报连接 fd 可读 → Loop 摘回调 → 回调写 `revents_` 并
   `resume` → `read_some` 的循环继续，`read(fd)` 拿到数据，`co_return`。
8. `echo_handler` 拿到 `nread > 0`，调 `write_all(std::move(input), ok)`：先试写，通常
   一次写完；若发送缓冲满则 `co_await WaitFd{fd, POLLOUT}` 挂起，等对端消费。
9. 循环回到第 6 步等下一行。客户端关闭 → `read(fd) == 0` → `nread == 0` → `break` →
   handler 返回 → `close_connection`：取回 fd、`close(fd)`、从连接表删除、连接数减一。
10. 全部连接关完，Ctrl-C / 发 SIGTERM → `request_shutdown()` 写 self-pipe → Loop 停止
    accept、对所有连接 `shutdown_read()`，20ms 一查，清零后 `loop_.stop()`，
    `server.run()` 返回，进程退出。

### 4.5 从这个最小例子走向生产

echo server 只有 I/O，没有重活。真实服务（SQL server）多了两件事：

1. **重活离线**：`co_await SubmitToService{&parse_service, work}` 把解析/执行投到专用
   ServiceThread，Loop 线程只做 I/O。`server.cpp` 里读请求轮询进 read-pool、写请求进
   write-service，`submitted == false` 时回 `server busy` 帧。
2. **优雅退出与连接上限**：`TcpServerOptions::max_connections` 控制并发上限，超出直接
   关新连接；`request_shutdown()` 给已有连接一段 grace 时间收尾。

给 echo server 演示"离线 + 背压"也很简单：

```cpp
#include "common/svrkit/service.h"

common::svrkit::ServiceThread echo_work("echo-work");
echo_work.start(64);   // 队列上限 64（队列必须有界）

Task echo_handler(std::shared_ptr<TcpConnection> conn) {
  while (true) {
    std::string input;
    ssize_t nread = 0;
    co_await conn->read_some(input, nread);
    if (nread <= 0) break;
    std::string result;                      // work 写协程帧局部量，安全
    common::svrkit::SubmitToService submit;
    submit.service = &echo_work;
    submit.work = [&] { result = std::string("echo:") + input; };
    co_await submit;
    if (!submit.submitted) break;            // 队列满：背压，放弃这条连接
    bool ok = false;
    co_await conn->write_all(std::move(result), ok);
    if (!ok) break;
  }
}
```

## 附录：速查表

| 术语 | 一句话 |
|---|---|
| 协程帧 | 编译器生成的状态机内存，含 promise、局部变量、挂起点编号 |
| `coroutine_handle` | 指向帧的句柄；`resume / destroy / done / promise` |
| `promise_type` | 协程与调用者的接口：`get_return_object / initial_suspend / final_suspend / yield_value / unhandled_exception` |
| awaiter 三方法 | `await_ready`（快速路径）、`await_suspend`（交出 handle）、`await_resume`（取结果） |
| `initial_suspend` | 决定函数体是否调用时就跑（svrkit 用 `suspend_always`：懒启动） |
| `final_suspend` | 决定结束后的动作（svrkit：对称转移给父协程，或 post 销毁顶层帧） |
| 水平触发 | 只要 fd 可读/可写就持续上报；配合"恢复后重试系统调用"不会丢事件 |
| `Loop` | 每线程事件循环，唯一允许 resume 协程的地方 |
| `post()` | 线程安全地往 Loop 投递动作，用 self-pipe 唤醒 poll |
| `ServiceThread` | 有界队列 + 专用线程，完成重活后 `caller_loop.post` 回跳 |
| 背压 | 队列满时 `submit()` 返回 false，由应用层把压力转成协议错误 |




