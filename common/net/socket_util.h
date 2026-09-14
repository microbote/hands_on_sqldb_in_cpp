// common/net/socket_util.h
//
// socket 写操作的两个平台差异，收在这里：
//   - **SIGPIPE**：对端已经关掉读方向时 `write()` 会先送 SIGPIPE，默认处置
//     直接把进程杀掉 —— 客户端半途消失就能打死服务器（这里踩过：测试进程
//     exit=141 = SIGPIPE）。修法两条并用：
//       * `send(..., MSG_NOSIGNAL)`（Linux 一定有；本机 macOS SDK 也定义了，
//         实测有效）；
//       * 建连时设 `SO_NOSIGPIPE` 兜底（给没有 MSG_NOSIGNAL 的平台）。
//     注意 **SO_NOSIGPIPE 对 AF_UNIX socketpair 无效**（macOS 上
//     setsockopt 直接 EINVAL，实测），所以 `socketpair` 的用例只能靠
//     MSG_NOSIGNAL 那条路；TCP 连接两条都生效。
//   - 其余平台差异（EINTR/EAGAIN 重试）留给调用方的循环，这里只保证
//     "一次写不杀进程"。
#pragma once

#include <cstddef>
#include <sys/socket.h>
#include <unistd.h>

namespace common::net {

// 建连/accept 之后调一次：让这个 fd 上的写不再产生 SIGPIPE。
// Linux 上没有 SO_NOSIGPIPE（靠 socket_write 的 MSG_NOSIGNAL），此时什么都不做。
inline void socket_suppress_sigpipe(int fd) {
#ifdef SO_NOSIGPIPE
  int on = 1;
  if (fd >= 0) {
    (void)::setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &on, sizeof(on));
  }
#else
  (void)fd;
#endif
}

// 写一次（不做重试）：失败时 errno 与 write() 语义一致。
// 对端已关闭时返回 -1 + EPIPE（而不是把进程打死）。
inline ssize_t socket_write(int fd, const char *data, size_t size) {
#ifdef MSG_NOSIGNAL
  return ::send(fd, data, size, MSG_NOSIGNAL);
#else
  return ::write(fd, data, size);
#endif
}

} // namespace common::net
