// common/svrkit/loop.h
//
// 每线程一个事件循环：common::net::Poller（平台相关的 kqueue/epoll/poll，见
// common/net/poller.h）+ 定时器最小堆 + 跨线程唤醒管道。
//
//   - 一个 Loop 归一条线程独占：`watch/unwatch/add_timer` 只能在 Loop
//   线程调用；
//   - `post()` 线程安全：把"继续执行"投进 Loop，并唤醒它的等待。
#pragma once

#include <atomic>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "common/net/poller.h"
#include "common/svrkit/task.h"

namespace common::svrkit {

class Loop {
public:
  explicit Loop(std::string name = "loop");
  ~Loop();

  Loop(const Loop &) = delete;
  Loop &operator=(const Loop &) = delete;

  // 跑事件循环直到 stop()（阻塞当前线程）
  void run();
  // 线程安全：请求退出（会唤醒 poll）
  void stop();
  bool stopped() const { return stop_.load(); }
  bool in_this_thread() const;

  // 线程安全：安排一个动作在 Loop 线程执行
  void post(std::function<void()> action);
  // 线程安全：启动一个协程（frame 由 Loop 管生命周期）
  void spawn(Task task);

  // 以下只能在 Loop 线程调用。
  // 关心某 fd 的读/写事件；同一 fd 只允许一个关注者（协程之间不共享 fd）
  void watch(int fd, short events, std::function<void(short)> on_events);
  void unwatch(int fd);
  // 定时器：delay_ms 之后执行一次
  void add_timer(int64_t delay_ms, std::function<void()> action);

  const std::string &name() const { return name_; }

private:
  struct Watch {
    short events = 0;
    std::function<void(short)> on_events;
  };

  void wakeup();
  void drain_wakeup();
  void run_due_timers();

  std::string name_;
  std::atomic<bool> stop_{false};

  std::unique_ptr<net::Poller> poller_; // 平台后端：kqueue / epoll / poll
  std::mutex mutex_;                          // 保护 pending_
  std::deque<std::function<void()>> pending_; // 跨线程投递的动作
  int wakeup_read_ = -1;
  int wakeup_write_ = -1;

  std::unordered_map<int, Watch> watches_;
  // 定时器：简单线性表（(deadline_us, action)），数量很少
  std::vector<std::pair<int64_t, std::function<void()>>> timers_;
};

} // namespace common::svrkit
