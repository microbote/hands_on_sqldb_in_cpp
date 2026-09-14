// common/net/poller.h
//
// 平台无关的 IO 多路复用后端（学 libco 的第一点：上层接口固定，底层按
// 平台条件编译换实现）：
//
//   __APPLE__  → kqueue          common/net/poller_kqueue.cpp
//   __linux__  → epoll（LT）     common/net/poller_epoll.cpp
//   任何平台   → poll(2) 兜底     common/net/poller_poll.cpp
//                （全平台都编译；平台后端创建失败时也退回它）
//
// 语义统一成 poll(2) 的词汇：**水平触发**、事件位是
// POLLIN/POLLOUT/POLLERR/POLLHUP/POLLNVAL —— 上层（Loop/WaitFd）只看到
// 这一套，不用关心底下是谁。epoll 特意**不开 EPOLLET**：边缘触发会改变
// "恢复后重试系统调用"的上层约定。
#pragma once

#include <poll.h>

#include <memory>

namespace common::net {

class Poller {
public:
  struct Event {
    int fd = -1;
    short revents = 0; // POLLIN/POLLOUT/POLLERR/POLLHUP/POLLNVAL 的位或
  };

  virtual ~Poller() = default;

  // 关注 fd 的可读(POLLIN)/可写(POLLOUT)；重复调用 = 修改关注集。
  // 失败（比如 fd 非法）返回 false。
  virtual bool watch(int fd, short events) = 0;
  virtual void unwatch(int fd) = 0;
  // 等事件：timeout_ms < 0 永久阻塞；返回事件数（0 = 超时），-1 = 错误
  // （errno 保留，EINTR 由调用方处理）。同一 fd 的多个事件位合并成一条。
  virtual int wait(int timeout_ms, Event *out, int max_events) = 0;

  // 按平台挑实现；平台后端创建失败时退回 poll
  static std::unique_ptr<Poller> create();
};

// 后端工厂（poller_poll 全平台都有；平台后端只在自己的平台编译）
std::unique_ptr<Poller> make_poll_poller();
#if defined(__APPLE__)
std::unique_ptr<Poller> make_kqueue_poller();
#elif defined(__linux__)
std::unique_ptr<Poller> make_epoll_poller();
#endif

} // namespace common::net
