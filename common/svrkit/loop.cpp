// common/svrkit/loop.cpp
#include "common/svrkit/loop.h"

#include <fcntl.h>
#include <poll.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <utility>

namespace common::svrkit {
namespace {

thread_local Loop *g_loop = nullptr;

int64_t now_us() {
  return std::chrono::duration_cast<std::chrono::microseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

// 唤醒管道必须**非阻塞**：drain 到"读不到为止"时，最后一次 read 要返回
// EAGAIN 而不是阻塞 —— 阻塞会把整个事件循环挂死（这里踩过，见 codex 日志）。
void make_nonblocking(int fd) {
  const int flags = ::fcntl(fd, F_GETFL, 0);
  if (flags >= 0) {
    ::fcntl(fd, F_SETFL, flags | O_NONBLOCK);
  }
}

} // namespace

Loop *&current_loop() { return g_loop; }

// ============================================================
// Task / awaitable 的胶水（都依赖 Loop，所以放在这里）
// ============================================================
Task Task::promise_type::get_return_object() {
  return Task{std::coroutine_handle<promise_type>::from_promise(*this)};
}

void Task::promise_type::FinalAwaiter::await_suspend(
    std::coroutine_handle<promise_type> handle) noexcept {
  // 嵌套 co_await：直接对称转交回调用者（同一个 Loop 线程，不绕一圈）
  if (handle.promise().continuation) {
    handle.promise().continuation.resume();
    return;
  }
  // spawn 出来的顶层协程：没人接，Loop 负责销毁 frame
  if (Loop *loop = current_loop(); loop != nullptr) {
    loop->post([handle] { handle.destroy(); });
  }
}

void Task::await_suspend(std::coroutine_handle<> caller) noexcept {
  handle_.promise().continuation = caller;
  handle_.resume(); // 立即开跑；它在第一个真正的等待点让出
}

void Task::await_resume() {
  if (handle_.promise().error) {
    std::rethrow_exception(handle_.promise().error);
  }
}

void WaitFd::await_suspend(std::coroutine_handle<> handle) {
  Loop *loop = current_loop();
  if (loop == nullptr) {
    return;
  }
  WaitFd *self = this; // 临时量在挂起期间一直活着（见 task.h）
  loop->watch(fd, events, [self, handle](short revents) {
    self->revents_ = revents;
    handle.resume();
  });
}

short WaitFd::await_resume() const { return revents_; }

void SleepFor::await_suspend(std::coroutine_handle<> handle) const {
  Loop *loop = current_loop();
  if (loop == nullptr) {
    return;
  }
  loop->add_timer(ms, [handle] { handle.resume(); });
}

// ============================================================
// Loop
// ============================================================
Loop::Loop(std::string name) : name_(std::move(name)) {
  int fds[2] = {-1, -1};
  if (::pipe(fds) == 0) {
    wakeup_read_ = fds[0];
    wakeup_write_ = fds[1];
    make_nonblocking(wakeup_read_);
    make_nonblocking(wakeup_write_);
  }
  poller_ = net::Poller::create();
  if (poller_ != nullptr && wakeup_read_ >= 0) {
    poller_->watch(wakeup_read_, POLLIN); // 常驻，不随 watches_ 摘挂
  }
}

Loop::~Loop() {
  if (wakeup_read_ >= 0) {
    ::close(wakeup_read_);
  }
  if (wakeup_write_ >= 0) {
    ::close(wakeup_write_);
  }
}

bool Loop::in_this_thread() const { return current_loop() == this; }

void Loop::post(std::function<void()> action) {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    pending_.push_back(std::move(action));
  }
  wakeup();
}

void Loop::spawn(Task task) {
  // 顶层协程：frame 由 Loop 负责（结束时 FinalAwaiter 里 post 一个 destroy）
  auto handle = task.release();
  if (!handle) {
    return;
  }
  post([handle] { handle.resume(); });
}

void Loop::stop() {
  stop_ = true;
  wakeup();
}

void Loop::watch(int fd, short events, std::function<void(short)> on_events) {
  watches_[fd] = Watch{events, std::move(on_events)};
  if (poller_ != nullptr) {
    (void)poller_->watch(fd, events);
  }
}

void Loop::unwatch(int fd) {
  watches_.erase(fd);
  if (poller_ != nullptr) {
    poller_->unwatch(fd);
  }
}

void Loop::add_timer(int64_t delay_ms, std::function<void()> action) {
  timers_.emplace_back(now_us() + delay_ms * 1000, std::move(action));
}

void Loop::wakeup() {
  if (wakeup_write_ < 0) {
    return;
  }
  const char byte = 'w';
  ssize_t ignored = ::write(wakeup_write_, &byte, 1);
  (void)ignored;
}

void Loop::drain_wakeup() {
  if (wakeup_read_ < 0) {
    return;
  }
  char buffer[64];
  while (::read(wakeup_read_, buffer, sizeof(buffer)) > 0) {
  }
}

void Loop::run_due_timers() {
  const int64_t now = now_us();
  std::vector<std::function<void()>> due;
  for (size_t i = 0; i < timers_.size();) {
    if (timers_[i].first <= now) {
      due.push_back(std::move(timers_[i].second));
      timers_.erase(timers_.begin() + static_cast<long>(i));
    } else {
      ++i;
    }
  }
  for (auto &action : due) {
    action(); // 回调里可能再加定时器（已经先摘出来了，安全）
  }
}

void Loop::run() {
  g_loop = this;
  while (!stop_) {
    // 1) 处理跨线程投递 + 到期定时器
    std::deque<std::function<void()>> pending;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      pending.swap(pending_);
    }
    for (auto &action : pending) {
      action();
    }
    run_due_timers();
    if (stop_) {
      break;
    }

    // 2) 超时 = 最近一个定时器（没有就 1s，兜底唤醒）
    int timeout_ms = 1000;
    int64_t earliest = 0;
    bool has_timer = false;
    for (const auto &[id, timer] : timers_) {
      if (!has_timer || id < earliest) {
        earliest = id;
        has_timer = true;
      }
    }
    if (has_timer) {
      const int64_t delta_us = earliest - now_us();
      timeout_ms =
          delta_us <= 0
              ? 0
              : static_cast<int>(std::min<int64_t>(delta_us / 1000 + 1, 1000));
    }

    // 3) 等事件：后端是平台相关的 kqueue/epoll/poll，事件词汇统一成 poll 的
    net::Poller::Event fired[64];
    const int ready =
        poller_ != nullptr ? poller_->wait(timeout_ms, fired, 64) : -1;
    if (ready < 0) {
      if (errno == EINTR) {
        continue;
      }
      break;
    }
    // 4) 派发 fd 事件：**先摘出回调再调用**（回调里可能重新 watch/关 fd）
    for (int i = 0; i < ready; ++i) {
      const int fd = fired[i].fd;
      const short revents = fired[i].revents;
      if (fd == wakeup_read_) {
        if ((revents & POLLIN) != 0) {
          drain_wakeup();
        }
        continue; // 唤醒管道常驻注册，不摘
      }
      if ((revents & POLLNVAL) != 0) {
        unwatch(fd); // fd 已被关：顺手摘掉，别反复报
        continue;
      }
      auto it = watches_.find(fd);
      if (it == watches_.end()) {
        continue;
      }
      auto callback = std::move(it->second.on_events);
      unwatch(fd);
      callback(revents);
    }
  }
  g_loop = nullptr;
}

} // namespace common::svrkit
