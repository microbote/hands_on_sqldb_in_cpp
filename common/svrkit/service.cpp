// common/svrkit/service.cpp
#include "common/svrkit/service.h"

#include <utility>

namespace common::svrkit {

ServiceThread::ServiceThread(std::string name) : name_(std::move(name)) {}

ServiceThread::~ServiceThread() { stop(); }

void ServiceThread::start(size_t queue_max) {
  queue_max_ = queue_max == 0 ? 1 : queue_max;
  worker_ = std::thread([this] { run(); });
}

void ServiceThread::stop() {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    stopping_ = true;
  }
  wake_.notify_all();
  if (worker_.joinable()) {
    worker_.join();
  }
}

bool ServiceThread::submit(std::function<void()> work, Loop *caller_loop,
                           std::coroutine_handle<> handle) {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (stopping_ || queue_.size() >= queue_max_) {
      return false;
    }
    queue_.push_back([work = std::move(work), caller_loop, handle] {
      work();
      if (handle == nullptr) {
        return; // 不需要通知（fire-and-forget）
      }
      if (caller_loop != nullptr) {
        caller_loop->post([handle] { handle.resume(); });
      } else {
        handle.resume();
      }
    });
  }
  wake_.notify_one();
  return true;
}

size_t ServiceThread::queue_size() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return queue_.size();
}

void ServiceThread::run() {
  while (true) {
    std::function<void()> work;
    {
      std::unique_lock<std::mutex> lock(mutex_);
      wake_.wait(lock, [this] { return stopping_ || !queue_.empty(); });
      if (queue_.empty()) {
        if (stopping_) {
          return;
        }
        continue;
      }
      work = std::move(queue_.front());
      queue_.pop_front();
    }
    work();
  }
}

} // namespace common::svrkit
