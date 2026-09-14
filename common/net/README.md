# common/net：跨平台网络原语

这一层只做**协议无关**的 fd/socket 能力，给 `common/svrkit`、阻塞式客户端、
后续 Raft transport 共用。这里不出现 SQL、Raft 或任何上层消息格式。

## 组件

| 文件 | 职责 |
|---|---|
| `poller.h` + `poller_kqueue.cpp` / `poller_epoll.cpp` / `poller_poll.cpp` | 平台 IO 多路复用：macOS 用 kqueue，Linux 用 epoll（水平触发），其他平台退回 `poll(2)`；对外统一使用 `POLLIN/POLLOUT/POLLERR/POLLHUP/POLLNVAL` 词汇 |
| `socket.h/.cpp` | `TcpSocket`：fd 所有权、TCP connect/listen、非阻塞、`TCP_NODELAY`、阻塞式读写 |
| `socket_util.h` | 单次 socket 写的平台差异，尤其是 SIGPIPE 防护（`MSG_NOSIGNAL` + `SO_NOSIGPIPE`） |

## 设计约束

- 事件语义固定为**水平触发**：等待方被恢复后必须重试 `read/write`，底层
  epoll 不启用 `EPOLLET`。
- 当前 TCP 地址接口只支持 IPv4 字面量；需要 IPv6 时应扩展
  `TcpSocket::connect/listen` 的地址解析，而不是把地址细节散到调用方。
- `TcpSocket` 是 move-only；`adopt(fd)` 之后 fd 归对象负责关闭。
- `socket_util.h` 只做"一次写不杀进程"；EINTR/EAGAIN 的等待和重试属于
  调用方（阻塞式在 `TcpSocket`，协程式在 `svrkit::TcpConnection`）。
