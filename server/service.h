// server/service.h
//
// `ServiceThread`：一条"专门干某类活"的线程 + 有界队列 + 完成后回原 Loop。
//
// 服务器里有三组实例：
//   - ParseService：解析（词法/语法层有进程级全局状态，只允许一个线程做）；
//   - WriteService：写语句 + 事务里的所有语句（悲观单写者的排队点）；
//   - ReadPool：N 条读线程（execution.read_threads），纯读语句轮询投进去，
//     读请求之间真正并发。
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

#include "loop.h"

namespace server {

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

} // namespace server
