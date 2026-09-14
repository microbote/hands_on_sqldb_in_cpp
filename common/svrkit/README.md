# common/svrkit：协议无关的协程服务器框架

`svrkit` 是给多个长连接服务复用的薄框架：SQL server 现在使用它，后续
Raft node 也可以只实现协议 handler，而不复制 accept、事件循环、连接所有权
和优雅退出逻辑。

## 分层

| 组件 | 职责 |
|---|---|
| `Task` / `WaitFd` / `SleepFor` | C++20 stackless coroutine 基础；结果通过引用参数写回 |
| `Loop` | 每线程事件循环：`common::net::Poller` + 定时器 + 跨线程 `post/spawn` |
| `ServiceThread` / `SubmitToService` | 有界队列 + 专用工作线程；任务完成后回到发起 Loop 恢复协程 |
| `TcpServer` / `TcpConnection` | TCP listen/accept、连接上限、非阻塞协程读写、attach 已建连接、连接计数、self-pipe 优雅退出 |

## Raft/SQL 共用的接入方式

应用层只提供一个连接处理器：

```cpp
common::svrkit::TcpServerOptions options;
options.max_connections = 256;

common::svrkit::TcpServer server(
    options,
    [](std::shared_ptr<common::svrkit::TcpConnection> conn)
        -> common::svrkit::Task {
      std::string input;
      ssize_t nread = 0;
      co_await conn->read_some(input, nread);
      if (nread > 0) {
        bool ok = false;
        co_await conn->write_all(handle_message(input), ok);
      }
      // 返回后 TcpServer 统一关闭 fd 并注销连接
    });

server.listen("127.0.0.1", 9000);
server.run();
```

## 边界与纪律

- `svrkit` **不定义消息格式**：SQL 的 `server/protocol.*` 仍留在 `server/`，
  Raft 可以定义自己的帧编解码。
- 应用层协议收尾帧（例如 SQL 的 `BYE`）由 handler 在返回前发送；fd 关闭由
  `TcpServer` 统一做。
- 一个连接协程只在一个 `Loop` 上跑；跨线程只允许 `Loop::post()`。
- 重活通过 `ServiceThread` 投出去，队列满要由应用层转成协议错误/背压。
- `request_shutdown()` 只写 self-pipe，可从信号处理函数调用；真正停止
  accept、唤醒连接和等待收尾都在 Loop 线程完成。
