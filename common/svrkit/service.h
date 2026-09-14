// common/svrkit/service.h
//
// `ServiceThread`：一条"专门干某类活"的线程 + 有界队列 + 完成后回原 Loop。
//
// 典型用途：协议解析、串行写、Raft apply、读请求 worker pool。它不关心
// 任务内容，只保证三件事：有界排队（背压）、任务离开 Loop 线程执行、完成后
// 回到发起 Loop 恢复协程。
//
// 用法（在协程里）：
//     Outcome outcome;
//     co_await SubmitToService{&parse_service, [&]{ outcome = do_work(); }};
//     // 恢复时 outcome 已经填好（fn 在服务线程上跑，恢复在发起线程上）
//
// 纪律：fn 里**只碰**协程帧上的局部量（或服务线程自己独占的东西），
// 不要碰"发起线程正在用的对象" —— 协程挂起期间那个对象没人动，
// 所以按引用捕获是安全的（同一时刻只有一个线程在跑这个协程）。
#pragma once

#include <condition_variable>
#include <coroutine>
#include <cstddef>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <thread>

#include "common/svrkit/loop.h"

namespace common::svrkit {

class SubmitToService;

class ServiceThread {
public:
  explicit ServiceThread(std::string name);
  ~ServiceThread();

  ServiceThread(const ServiceThread &) = delete;
  ServiceThread &operator=(const ServiceThread &) = delete;

  // 起线程；queue_max = 队列上限（超过 submit 返回 false，交给调用方报"忙"）
  void start(size_t queue_max);
  void stop();

  // 线程安全：投递一个活；成功返回 true。
  // 完成后在 caller_loop 上 `post` 恢复 handle。
  bool submit(std::function<void()> work, Loop *caller_loop,
              std::coroutine_handle<> handle);

  size_t queue_size() const;
  const std::string &name() const { return name_; }

private:
  void run();

  std::string name_;
  size_t queue_max_ = 0;
  mutable std::mutex mutex_;
  std::condition_variable wake_;
  std::deque<std::function<void()>> queue_;
  bool stopping_ = false;
  std::thread worker_;
};

// ============================================================
// co_await SubmitToService{&service, [&]{ ... }}
// ============================================================
class SubmitToService {
public:
  ServiceThread *service = nullptr;
  std::function<void()> work;
  bool submitted = false; // 恢复后：false = 队列满，被拒了

  bool await_ready() const noexcept { return false; }
  void await_suspend(std::coroutine_handle<> handle) {
    submitted = service->submit(std::move(work), current_loop(), handle);
    if (!submitted) {
      handle.resume(); // 没排上：立刻恢复，让调用方看到 submitted == false
    }
  }
  void await_resume() const {}
};

} // namespace common::svrkit
