// server/server.h
//
// sqldb 服务器：一个进程 = 一份 KVStore + N 条连接协程 + 1 条 parse 服务线程
// + 1 条写线程。
//
//   Loop(主线程/poll)                 ParseService          WriteService
//     ├─ acceptor 协程                  解析（全局态）         写语句 /
//     事务里的语句 └─ connection 协程 ── SQL ──►  co_await 提交 ──► ...
//            ▲                                              │
//            └──────────── Loop::post(resume) ◄─────────────┘
//
// 路由规则（与设计一致）：`SELECT` 且不在事务里 → 就地执行（读池 = Loop
// 线程）；
// **其余一切（含事务里的 SELECT）→ WriteService**。执行 SQL 是同步的，
// 协程只在网络 I/O、提交服务、定时器三处让出。
#pragma once

#include <atomic>
#include <cstdint>
#include <expected>
#include <memory>
#include <string>

#include "config.h"
#include "loop.h"
#include "service.h"
#include "session/session.h"
#include "storage/kv_engine/kv_engine.h"

namespace server {

class Server {
public:
  Server(Config config, std::shared_ptr<kv::KVStore> store);
  ~Server();

  Server(const Server &) = delete;
  Server &operator=(const Server &) = delete;

  // 建监听 socket；失败返回信息
  std::expected<void, std::string> listen();
  // 跑事件循环（阻塞）；stop() 可以从别的线程/信号处理里调
  void run();
  void stop() { loop_.stop(); }

  // 供测试：等所有连接结束（不是必须的，event loop 停掉即可）
  int port() const { return port_; }
  kv::KVStore &store() { return *store_; }
  const Config &config() const { return config_; }
  Loop &loop() { return loop_; }
  ServiceThread &parse_service() { return parse_service_; }
  ServiceThread &write_service() { return write_service_; }
  size_t connection_count() const { return connections_.load(); }

private:
  // 接受连接（协程）：accept 一个就 spawn 一个连接协程
  Task accept_loop();
  // 一条连接：HELLO → 循环(读帧 → 处理 → 写响应) → BYE
  Task serve_connection(int fd);

  Config config_;
  std::shared_ptr<kv::KVStore> store_;
  Loop loop_;
  ServiceThread parse_service_;
  ServiceThread write_service_;
  int listen_fd_ = -1;
  int port_ = 0;
  std::atomic<size_t> connections_{0};
};

} // namespace server
