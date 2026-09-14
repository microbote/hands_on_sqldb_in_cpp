// tests/test_svrkit/test_service.cpp
//
// ServiceThread：活确实跑在服务线程上、完成后在发起 Loop 上恢复协程、
// 队列上限会拒绝（背压）。
#include "test_framework.h"

#include "common/svrkit/loop.h"
#include "common/svrkit/service.h"

#include <atomic>
#include <chrono>
#include <string>
#include <thread>

namespace {

std::thread::id loop_thread_id() { return std::this_thread::get_id(); }

} // namespace

TEST(ServiceThread, RunsWorkOffThreadAndResumesOnLoop) {
  common::svrkit::Loop loop("test-loop");
  common::svrkit::ServiceThread service("test-service");
  service.start(8);

  std::thread::id main_thread = std::this_thread::get_id();

  // 协程：提交一个活 -> 挂起 -> 服务线程跑完 -> 回 Loop 恢复
  struct Result {
    std::atomic<bool> work_ran{false};
    std::atomic<bool> resumed{false};
    std::thread::id service_thread;
  } result;

  auto body = [&]() -> common::svrkit::Task {
    common::svrkit::SubmitToService submit;
    submit.service = &service;
    submit.work = [&] {
      result.service_thread = std::this_thread::get_id();
      result.work_ran = true;
    };
    co_await submit;
    CHECK(submit.submitted);
    result.resumed = true;
    loop.stop();
  };
  loop.spawn(body());
  loop.run();
  service.stop();

  CHECK(result.work_ran.load());
  CHECK(result.resumed.load());
  CHECK(result.service_thread != main_thread); // 活跑在服务线程上
  CHECK(result.service_thread != loop_thread_id());
}

TEST(ServiceThread, QueueLimitRejectsWhenFull) {
  common::svrkit::Loop loop("test-loop-full");
  common::svrkit::ServiceThread service("test-service-full");
  service.start(1); // 队列里最多排 1 个（正在执行的那条不算）

  std::atomic<bool> first_started{false};
  std::atomic<bool> release{false};
  std::atomic<int> ran{0};

  // 第 1 个活占住服务线程（等 release），此时队列是空的
  CHECK(service.submit(
      [&] {
        first_started = true;
        while (!release.load()) {
          std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        ++ran;
      },
      &loop, {}));
  while (!first_started.load()) {
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  // 第 2 个进队列
  CHECK(service.submit([&] { ++ran; }, &loop, {}));
  // 第 3 个撞上限 -> 被拒（这就是背压）
  CHECK(!service.submit([&] { ++ran; }, &loop, {}));

  release = true;
  while (ran.load() < 2) {
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  service.stop();
  CHECK_EQ(ran.load(), 2);
}

TEST(Loop, TimersAndPostedActionsRun) {
  common::svrkit::Loop loop("test-loop-timers");
  std::vector<std::string> order;
  loop.post([&] { order.push_back("posted"); });

  auto body = [&]() -> common::svrkit::Task {
    co_await common::svrkit::SleepFor{20};
    order.push_back("timer");
    co_await common::svrkit::SleepFor{5};
    order.push_back("timer2");
    loop.stop();
  };
  loop.spawn(body());
  loop.run();

  CHECK_EQ(order.size(), size_t{3});
  if (order.size() == 3) {
    CHECK_EQ(order[0], std::string("posted"));
    CHECK_EQ(order[1], std::string("timer"));
    CHECK_EQ(order[2], std::string("timer2"));
  }
}
