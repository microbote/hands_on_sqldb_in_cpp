// common/svrkit/task.h
//
// 最小协程基础设施（C++20 stackless coroutine）。
//
// 为什么只有一个返回类型 `Task<void>`：svrkit 里所有协程的"结果"都通过
// **引用参数**写回（`Task<void> read_frame(Conn&, Frame& out, bool& ok)`），
// 调用方也是协程、帧一直活着 —— 省掉一整套 `Task<T>` 模板与嵌套恢复的坑。
//
// 让出点只有三处：
//   1) `WaitFd`  —— 等 socket 可读/可写（poll
//   语义：**水平触发**，恢复后重试系统调用）； 2) `SleepFor` —— 定时器； 3)
//   `ServiceThread::submit()`（见 service.h）—— 把重活交给专用服务线程，
//   完成后由对方 `Loop::post()` 回本线程再恢复。
//
// 线程模型：一个 Task 只在**一个** Loop 上跑；跨线程只允许 `Loop::post()` 投递
// "继续执行"的动作。因此"同一时刻只有一个线程碰这个协程帧"是构造性成立的。
#pragma once

#include <coroutine>
#include <cstdint>
#include <exception>
#include <utility>

// WaitFd::await_suspend 的实现要用 poll 的事件常量（loop.cpp 里 include，
// 这里只保证结构体声明完整）。

namespace common::svrkit {

class Loop;

// 当前线程正在跑的 Loop（Loop::run() 线程内设置）
Loop *&current_loop();

// ============================================================
// Task<void>
// ============================================================
class Task {
public:
  struct promise_type {
    std::coroutine_handle<> continuation{}; // 本协程结束后 resume 谁
    std::exception_ptr error;

    Task get_return_object();
    std::suspend_always initial_suspend() noexcept { return {}; }
    struct FinalAwaiter {
      bool await_ready() noexcept { return false; }
      void await_suspend(std::coroutine_handle<promise_type> handle) noexcept;
      void await_resume() noexcept {}
    };
    FinalAwaiter final_suspend() noexcept { return {}; }
    void return_void() {}
    void unhandled_exception() { error = std::current_exception(); }
  };

  Task() = default;
  explicit Task(std::coroutine_handle<promise_type> handle) : handle_(handle) {}
  Task(const Task &) = delete;
  Task &operator=(const Task &) = delete;
  Task(Task &&other) noexcept : handle_(std::exchange(other.handle_, {})) {}
  Task &operator=(Task &&other) noexcept {
    if (this != &other) {
      if (handle_) {
        handle_.destroy();
      }
      handle_ = std::exchange(other.handle_, {});
    }
    return *this;
  }
  ~Task() {
    if (handle_) {
      handle_.destroy();
    }
  }

  // `spawn()` 用：交出 frame 的所有权（交给 Loop 管生命周期）
  std::coroutine_handle<promise_type> release() {
    return std::exchange(handle_, {});
  }

  // 被 co_await：先挂起调用者，再启动本协程
  bool await_ready() const noexcept { return false; }
  void await_suspend(std::coroutine_handle<> caller) noexcept;
  void await_resume();

private:
  std::coroutine_handle<promise_type> handle_{};
};

// ============================================================
// WaitFd：等 fd 变成可读/可写（poll 水平触发，恢复后重试系统调用）
// ============================================================
struct WaitFd {
  int fd = -1;
  short events = 0; // POLLIN / POLLOUT

  bool await_ready() const noexcept { return fd < 0; }
  void await_suspend(std::coroutine_handle<> handle);
  // 返回 poll 报告的事件（含 POLLERR/POLLHUP）；调用方据此决定读/写/关闭
  short await_resume() const;

  // 由 Loop 在事件到达时写入（awaitable 临时量在挂起期间一直活着）
  mutable short revents_ = 0;
};

// ============================================================
// SleepFor：等一段时间（毫秒）
// ============================================================
struct SleepFor {
  int64_t ms = 0;

  bool await_ready() const noexcept { return ms <= 0; }
  void await_suspend(std::coroutine_handle<> handle) const;
  void await_resume() const {}
};

} // namespace common::svrkit
