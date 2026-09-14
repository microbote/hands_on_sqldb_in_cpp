# common/svrkit 设计与协程原理

这份文档解释 `common/svrkit` 为什么这样设计，以及它的 C++20 stackless
coroutine 到底是怎么工作的。目标是让读者能从源码逐行验证它的线程边界、
协程帧生命周期和挂起/恢复路径，而不是把它当成黑盒。

## 1. 设计目标

`svrkit` 是一个协议无关的长连接服务器骨架，当前给 SQL server 使用，后续 Raft
transport 也可以复用。它只解决四类问题：

1. **I/O 多路复用**：大量 fd 上的读、写、accept 不能每个连接一个线程。
2. **协程化同步代码**：连接处理逻辑写成顺序的 `read -> process -> write`，
   但等待 fd 或定时器时不阻塞线程。
3. **重活离开 I/O 线程**：解析、写 KV、Raft apply 等交给专用 `ServiceThread`。
4. **统一连接生命周期与退出**：连接上限、fd 所有权、优雅退出由框架处理。

它刻意不做：

- 消息协议编解码；
- SQL 语义或 Raft 语义；
- 用户级线程调度器；
- M:N 协程池；
- 跨多个 `Loop` 迁移同一个协程。

## 2. 总体分层

```text
应用 handler（SQL / Raft）
        │  Task，顺序代码，包含 co_await
        ▼
Task + WaitFd / SleepFor / SubmitToService
        │  await_suspend 把 coroutine handle 交给调度源
        ▼
Loop（每线程一个）
        │  Poller 事件 / timer 到期 / 跨线程 post
        ▼
Poller（kqueue / epoll / poll）
        │  fd readiness
        ▼
TcpServer / TcpConnection
```

`ServiceThread` 在这个模型里是一个“离线执行源”：

```text
Loop 线程上的协程
  co_await SubmitToService
      协程挂起，work 进入 ServiceThread 队列
          worker 线程执行 work
              caller_loop.post(resume)
                  原 Loop 恢复协程
```

## 3. Poller：先把 OS 差异藏掉

`common/net/Poller` 的抽象很小：

```cpp
watch(fd, POLLIN / POLLOUT);
unwatch(fd);
wait(timeout, out_events, max_events);
```

后端按平台选择：

| 平台 | 后端 | 说明 |
|---|---|---|
| macOS | kqueue | 转换成 poll 事件词汇 |
| Linux | epoll | 明确不用 `EPOLLET`，保持水平触发 |
| 其他/创建失败 | poll | 全平台兜底 |

关键约定是 **level-triggered**：只要 fd 仍然可读/可写，下一轮 `wait()` 还会报。
这让上层可以采用最简单也最容易验证的模式：

```cpp
while (true) {
  n = read(fd);
  if (n > 0) return;
  if (errno == EAGAIN) {
    co_await WaitFd{fd, POLLIN};
    continue;
  }
}
```

恢复后重试系统调用；如果又遇到 `EAGAIN`，再注册等待。因为 `Loop` 恢复回调前会
`unwatch(fd)`，不会出现同一个 fd 的旧回调反复触发的问题。

## 4. Loop：每线程一个调度源

`Loop` 不是通用线程池。它属于且只属于调用 `run()` 的线程：

```cpp
void Loop::run() {
  g_loop = this;       // thread_local
  while (!stop_) {
    drain pending posts;
    run due timers;
    poller_->wait(timeout);
    dispatch fd events;
  }
  g_loop = nullptr;
}
```

### 4.1 三种唤醒来源

`Loop` 只有三种会让某个协程继续执行的事件：

1. **fd 事件**：`WaitFd::await_suspend` 调用 `Loop::watch`，事件到达时回调
   `handle.resume()`。
2. **定时器到期**：`SleepFor::await_suspend` 调用 `Loop::add_timer`，到点后
   `handle.resume()`。
3. **跨线程投递**：`Loop::post(action)` 把 action 放入 mutex 保护的队列，并写
   self-pipe 唤醒 `poll`。`ServiceThread` 完成后用它恢复协程。

### 4.2 fd 与回调的所有权

`Loop::watches_` 是：

```cpp
unordered_map<int, Watch>
```

其中 `Watch` 包含 `events` 和一个 `on_events` 回调。规则：

- 同一个 fd 只有一个关注者；
- 协程之间不共享 fd；
- `watch()` 只能在 Loop 线程调用；
  - 这不是线程安全 API；
  - 跨线程只能 `post()` 回 Loop 再调用。

事件派发时先取出回调并 `unwatch(fd)`，再执行回调：

```cpp
auto callback = std::move(it->second.on_events);
unwatch(fd);
callback(revents);
```

这样回调里可以重新 `watch`、关闭 fd、或启动别的逻辑，不会修改正在遍历的
`watches_`。

### 4.3 跨线程 post

`post()` 是少数线程安全入口：

```cpp
void Loop::post(std::function<void()> action) {
  {
    lock_guard lock(mutex_);
    pending_.push_back(std::move(action));
  }
  wakeup();   // write one byte to self-pipe
}
```

self-pipe 两端都是非阻塞。`run()` 醒来后先 drain pending，再处理 fd/timer。
这就是所有跨线程交互的收口点。

### 4.4 定时器实现

当前实现是 `vector<(deadline_us, action)>`，到期时线性扫描并摘出回调再执行。
它不是堆，尽管头文件早期注释写过“最小堆”。这个实现适合当前数量少的 watchdog /
graceful shutdown 定时器；如果以后 Raft 大量使用定时器，应该改成最小堆或
时间轮，并同步修正注释。

## 5. Task：C++20 stackless coroutine 的最小封装

这是最容易看不懂的部分。这里逐个字段解释。

### 5.1 协程调用为什么不会立刻执行

返回 `Task` 的函数：

```cpp
Task handler(TcpConnection &conn) {
  co_await conn.read_some(...);
}
```

编译器会生成一个 coroutine frame，里面保存：

- 函数局部变量；
- `promise_type`；
- 挂起点，也就是“下次从哪里继续”；
- `Task` 对象中的 `coroutine_handle`。

`Task::promise_type::initial_suspend()` 返回 `std::suspend_always`：

```cpp
std::suspend_always initial_suspend() noexcept { return {}; }
```

所以调用 `handler(...)` 只创建协程帧并立刻挂起，不执行函数体。执行必须由
`handle.resume()` 触发。

### 5.2 Task 只返回 void

`svrkit` 没有 `Task<T>`。结果通过引用参数写回：

```cpp
Task read_some(std::string &buffer, ssize_t &nread);
```

这样做的原因：

- 调用方也是协程，局部变量和引用在挂起期间一直活着；
- 避免实现一整套 `Task<T>`、嵌套返回值和异常传播模板；
- I/O 路径大多是多个输出值：字节数、ok、错误状态，引用参数更自然。

### 5.3 continuation：子协程结束后恢复谁

`promise_type` 中有：

```cpp
std::coroutine_handle<> continuation;
```

当父协程写：

```cpp
co_await child();
```

`Task::await_suspend(caller)` 做：

```cpp
child.promise().continuation = caller;
child.resume();    // 立即执行 child，直到第一个真正等待点
```

也就是说：

1. 父协程先挂起；
2. child 记住父协程的 handle；
3. child 立即开始执行；
4. child 遇到 `WaitFd` / `SleepFor` / `SubmitToService` 再挂起；
5. child 完成时通过 `FinalAwaiter` 恢复父协程。

`Task::FinalAwaiter` 是“协程结束”的挂起点：

```cpp
if (handle.promise().continuation) {
  continuation.resume();
  return;
}
// 顶层协程：Loop 负责销毁 frame
loop->post([handle] { handle.destroy(); });
```

所以子协程结束不会绕回事件循环，而是对称转移回父协程；只有顶层协程才由
`Loop` 调度销毁。

### 5.4 异常

`unhandled_exception()` 保存 `std::exception_ptr`。父协程在
`Task::await_resume()` 里重新抛出：

```cpp
void Task::await_resume() {
  if (handle_.promise().error) {
    std::rethrow_exception(handle_.promise().error);
  }
}
```

这就是 `TcpServer::run_connection()` 能捕获 handler 异常的原因：

```cpp
try {
  co_await handler_(connection);
} catch (...) {
  log(...);
}
```

### 5.5 生命周期与所有权

`Task` 是 move-only RAII：

- 持有 handle 就负责析构时 `destroy()`；
- `release()` 把所有权交给 `Loop::spawn()`；
- 顶层协程完成后由 `FinalAwaiter` post 一个 `destroy()` 动作；
- `co_await` 的 child Task 是临时对象，其生命周期覆盖整个 await 表达式，
  包括挂起期间和恢复后。

不要把同一个 Task 同时交给两个 owner，也不要在协程仍挂起时销毁它引用的
外部对象。

## 6. WaitFd：fd 等待是如何恢复协程的

使用点：

```cpp
co_await WaitFd{fd, POLLIN};
```

C++ 会在这个 await 表达式上调用三个方法：

### await_ready

```cpp
return fd < 0;
```

正常 fd 不 ready，进入挂起流程。无效 fd 会立即继续，这可能造成忙等，因此
调用方必须保证 fd 有效。

### await_suspend

```cpp
loop->watch(fd, events, [self, handle](short revents) {
  self->revents_ = revents;
  handle.resume();
});
```

这里做两件事：

1. 把当前协程 handle 存进 Loop 的 fd 回调；
2. 当前协程挂起，控制权回到 Loop。

`self` 指向 `WaitFd` 临时对象。这个临时对象在 await 表达式完成前不会销毁；
协程挂起时表达式尚未完成，所以事件回调里访问 `self->revents_` 是有效的。

### await_resume

```cpp
return revents_;
```

事件到达时 Loop 回调先写入 `revents_`，再 `handle.resume()`。恢复后调用方拿到
`POLLIN/POLLOUT/POLLERR/POLLHUP` 等事件位。

### 和非阻塞 I/O 的配合

`TcpConnection::read_some` 的循环是：

```text
read(fd)
  ├─ got > 0      → 返回
  ├─ got == 0     → 对端关闭
  ├─ EAGAIN       → co_await WaitFd(POLLIN)，恢复后 continue 重试
  ├─ EINTR        → continue
  └─ 其他错误     → 返回 -1
```

`write_all` 同理，只是等待 `POLLOUT`。因此一个连接在等待 I/O 时不占线程，
只占一个 coroutine frame 和一个 fd watch。

## 7. SleepFor：定时器等待

`SleepFor{ms}` 的 `await_suspend` 很简单：

```cpp
loop->add_timer(ms, [handle] { handle.resume(); });
```

它不参与 fd watch。Loop 的 `run_due_timers()` 会把到期回调先摘出来再执行，
因此回调里可以新增定时器。

## 8. ServiceThread：重活如何离线并回跳

### 8.1 SubmitToService

在协程里使用：

```cpp
int outcome = 0;
SubmitToService submit;
submit.service = &parse_service;
submit.work = [&] {
  outcome = parse(input);  // 跑在 service 线程
};
co_await submit;
// 这里已回到原 Loop，outcome 已填好
```

`SubmitToService::await_suspend` 做：

```cpp
submitted = service->submit(work, current_loop(), handle);
if (!submitted) {
  handle.resume();    // 队列满：立即恢复，让调用方处理 Busy
}
```

### 8.2 worker 侧

`ServiceThread::submit()` 把三样东西打包进队列：

- `work`
- `caller_loop`
- `caller coroutine handle`

worker 执行：

```cpp
work();
caller_loop->post([handle] { handle.resume(); });
```

所以恢复一定发生在原 Loop，不在 worker 线程。这保证了同一个协程始终只被
同一个 Loop 恢复。

### 8.3 线程安全边界

这是最重要的规则：

```text
ServiceThread 的 work 只能碰：
  1. 协程帧上的局部变量；
  2. 该 service 线程独占的对象；
  3. 明确加锁的共享对象。

不能碰：
  1. Loop 正在管理的 fd/callback；
  2. 另一个未挂起协程正在使用的对象；
  3. 假设“Loop 不会同时访问”的任意共享状态。
```

协程挂起期间，没有人会执行这个协程帧，因此 work 通过引用写局部结果是安全的。
但“这个协程不跑”不等于“整个 Loop 线程都停了”，Loop 还可能运行其他回调。

### 8.4 背压

队列上限不是性能优化，而是正确性边界：

```cpp
if (queue_.size() >= queue_max_) {
  return false;
}
```

队列满时 submit 失败，协程立即恢复，`SubmitToService::submitted == false`。
应用层必须把它转换成协议错误、等待或拒绝，不能无限堆任务。

## 9. TcpServer：连接生命周期

### 9.1 listen 与 accept

`TcpServer::run()`：

1. watch shutdown self-pipe；
2. 如果有 listen socket，spawn `accept_loop()`；
3. 进入 `Loop::run()`。

`accept_loop()` 等待 `POLLIN`，然后循环 `accept` 直到 `EAGAIN`。每个新 fd：

1. 检查连接上限，超限直接关闭；
2. `TcpSocket::adopt(fd)` 接管所有权；
3. `attach_connection(fd)` 设置非阻塞、抑制 SIGPIPE；
4. 创建 `TcpConnection`；
5. `loop.spawn(run_connection(connection))`。

### 9.2 连接协程

```cpp
Task TcpServer::run_connection(shared_ptr<TcpConnection> connection) {
  try {
    co_await handler_(connection);
  } catch (...) {
    log(...);
  }
  close_connection(connection);
}
```

`shared_ptr` 同时被 server 的 `connections_list_` 和协程帧持有。handler
返回或抛异常后，框架统一：

1. 从 `TcpConnection` 中取出 fd 并置为 -1；
2. `close(fd)`；
3. 从连接列表删除；
4. 计数减一。

协议层如果需要发送 `BYE`，必须在 handler 返回前完成；fd 关闭始终由框架做。

### 9.3 优雅退出

`request_shutdown()` 只写一个字节到 self-pipe，因此可以从信号处理函数调用。
Loop 醒来后执行 `begin_graceful_shutdown()`：

1. 停止 accept：unwatch 并关闭 listen socket；
2. 对所有连接 `mark_stopping()` 和 `shutdown_read()`；
3. 每 20ms 检查连接数；
4. 全部退出后 `loop.stop()`；
5. 超过 grace 时间仍退出，只是记录 warn。

`shutdown_read()` 会让阻塞在 `WaitFd(POLLIN)` 上的读协程醒来，读到 EOF；
写方向仍保留，让应用能发送错误帧或收尾帧。

## 10. 一个完整请求的时序

假设 SQL 连接已经建立：

```text
Loop 线程
  TcpConnection::read_some
    read(fd) -> EAGAIN
    WaitFd(fd, POLLIN)
      await_suspend
        Loop.watch(fd, callback)
      coroutine A 挂起
  Loop 继续处理其他 fd/timer

client 写入数据
  Poller 返回 fd readable
  Loop 取出 callback 并 unwatch(fd)
  callback:
    WaitFd.revents_ = POLLIN
    coroutine A resume
  read_some:
    read(fd) -> n bytes
    co_return

handler
  co_await SubmitToService
    coroutine A 挂起
    work 进 ServiceThread 队列

ServiceThread
  执行 parse/executor 等 work
  caller_loop.post(resume A)

Loop 线程
  post 队列 drain
  coroutine A resume
  write response
```

这期间线程没有阻塞在单个连接上；每个等待点都显式交出控制权。

## 11. 验证正确性的不变量

如果 review 或测试 svrkit，应重点验证以下不变量。

### I1：一个协程只属于一个 Loop

- 顶层协程由某个 `Loop::spawn()` 启动；
- `WaitFd` / `SleepFor` 恢复发生在同一个 Loop；
- `ServiceThread` 完成后 `post` 回原 Loop；
- 不允许把 handle 交给另一个 Loop 直接 resume。

### I2：Loop 私有 API 只在 Loop 线程调用

`watch/unwatch/add_timer` 不是线程安全的。跨线程必须 `post()`。

### I3：同一 fd 同时只有一个 watcher

否则 `unordered_map<int, Watch>` 的覆盖会丢失旧回调。框架通过“连接协程独占 fd”
保证这一点。

### I4：事件派发前先摘回调

回调可能修改 `watches_`，因此必须先 `move` 出 callback，再 `unwatch`，最后调用。

### I5：非阻塞系统调用 + 水平触发 + 恢复后重试

这是避免丢事件和忙等的核心组合。

### I6：Task frame 生命周期必须覆盖挂起

顶层 Task 交给 Loop；child Task 由 await 表达式持有；不能让外部引用对象先死。

### I7：Service work 不访问 Loop 线程正在管理的状态

离线 work 只写协程帧局部结果或线程独享状态。

### I8：队列必须有界

`ServiceThread` 的 queue 满必须可见地失败，让应用层实现背压。

## 12. 现有测试如何覆盖这些不变量

`tests/test_svrkit` 不是只测 API 表面，而是测关键行为：

| 测试 | 验证点 |
|---|---|
| `TcpSocket.AdoptedSocketPairSendsBothWays` | 非阻塞 socket 基础读写 |
| `TcpServer.AttachedConnectionRunsHandlerAndFrameworkClosesIt` | 协程 handler、读写、框架关 fd、连接计数 |
| `TcpServer.GracefulShutdownWakesHandlerAndStopsLoop` | self-pipe 停止、shutdown_read 唤醒、连接清零后停 Loop |
| `TcpServer.DestructorClosesAttachedConnectionEvenBeforeRun` | fd 所有权和析构关闭 |
| `ServiceThread.RunsWorkOffThreadAndResumesOnLoop` | work 在 worker 线程，恢复在 Loop |
| `ServiceThread.QueueLimitRejectsWhenFull` | 有界队列与背压 |
| `Loop.TimersAndPostedActionsRun` | post、timer、协程恢复顺序 |

如果怀疑协程正确性，优先加这些形状的测试：

1. 多连接交错读写，验证恢复到正确 handler；
2. 同一 fd 回调中重新 watch；
3. Service work 完成时同时有 fd 事件，验证 Loop 线程串行恢复；
4. handler 抛异常，验证 frame 释放和连接关闭；
5. shutdown 时多个连接同时等待读写。

## 13. 已知限制和后续改进

1. `Loop` 的 timers 目前是线性 vector，不适合大量定时器。
2. `WaitFd` 在 `current_loop() == nullptr` 时不排事件，直接恢复；这是误用，
   将来可以改成错误路径。
3. `ServiceThread` 的 work 抛异常会终结 worker 线程，调用方必须保证 work 不抛，
   或自行 catch。
4. `TcpServer::close_all_connections()` 是硬停止路径；正常退出依赖 handler 在
   grace 时间内返回。
5. `Loop::spawn()` 在 Loop 已停止后仍会 post，但没有人执行；不要在停止后再
   spawn 长生命周期任务。

这些限制不影响当前 SQL server 的使用方式，但 Raft 接入前应至少修正/讨论
第 2、3 点，因为 Raft runtime 对错误路径更敏感。
