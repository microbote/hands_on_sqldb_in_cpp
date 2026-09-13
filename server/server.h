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
#include <mutex>
#include <string>
#include <vector>

#include "config.h"
#include "loop.h"
#include "service.h"
#include "session/session.h"
#include "storage/kv_engine/kv_engine.h"

namespace server {

// 运行期计数（测试/运维用）：只增不减，读的时候不强求一致
struct Metrics {
  std::atomic<uint64_t> connections_total{0}; // 累计接受过的连接数
  std::atomic<uint64_t> statements{0};        // 累计执行的语句数
  std::atomic<uint64_t> errors{0};            // 返回 ERROR 帧的语句数
  std::atomic<uint64_t> rows_sent{0};         // 累计发出的结果行数
  std::atomic<uint64_t> idle_timeouts{0};     // 因空闲/事务空闲被断开的连接数
  std::atomic<uint64_t> write_queue_rejected{0}; // 写队列满被拒的次数
  std::atomic<uint64_t> parse_queue_rejected{0}; // 解析队列满被拒的次数
};

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
  // 立刻停（测试/异常路径用）：不等连接收尾
  void stop() { loop_.stop(); }

  // **优雅退出**：停止接受新连接 -> 通知所有连接收尾（发 BYE）-> 等它们退出
  // （有上限）-> 停事件循环。可以从信号处理里安全调用（内部只 write
  // 一根管道）。
  void request_shutdown();

  // 把一条**已经建立**的连接挂到事件循环上（accept 之外的入口：
  // socketpair 测试、或在别处 accept 之后转交进来）。用 socketpair 测试
  // 空闲超时/优雅退出时用它。
  void attach_connection(int fd);

  const Metrics &metrics() const { return metrics_; }
  bool shutting_down() const { return stopping_.load(); }

  // 供测试：等所有连接结束（不是必须的，event loop 停掉即可）
  int port() const { return port_; }
  kv::KVStore &store() { return *store_; }
  const Config &config() const { return config_; }
  Loop &loop() { return loop_; }
  ServiceThread &parse_service() { return parse_service_; }
  ServiceThread &write_service() { return write_service_; }
  size_t connection_count() const { return connections_.load(); }

private:
  // 一条连接的状态：定时器回调与协程共享（定时器只写这些原子量 + 关读方向）
  struct ConnState {
    int fd = -1;
    std::atomic<bool> timed_out{false};
    std::atomic<bool> stopping{false};
    std::atomic<uint64_t> generation{0}; // 每条语句 +1，让旧定时器失效
  };

  // 接受连接（协程）：accept 一个就 spawn 一个连接协程
  Task accept_loop();
  // 一条连接：HELLO → 循环(读帧 → 处理 → 写响应) → BYE
  Task serve_connection(int fd, std::shared_ptr<ConnState> state);

  // 只在 loop 线程调用
  void begin_graceful_shutdown();
  void log(const char *level, const std::string &message) const;
  void register_connection(const std::shared_ptr<ConnState> &state);
  void unregister_connection(int fd);
  // 空闲看门狗：到点就把读方向关掉（协程会醒来 -> 发 ERROR -> 收尾）
  void arm_idle_watchdog(const std::shared_ptr<ConnState> &state, bool in_tx);

  Config config_;
  std::shared_ptr<kv::KVStore> store_;
  Loop loop_;
  ServiceThread parse_service_;
  ServiceThread write_service_;
  int listen_fd_ = -1;
  int port_ = 0;
  std::atomic<size_t> connections_{0};
  std::atomic<bool> stopping_{false};
  Metrics metrics_;

  // 信号 -> 事件循环：self-pipe（write() 是 async-signal-safe 的）
  int signal_read_ = -1;
  int signal_write_ = -1;

  std::mutex connections_mutex_;
  std::vector<std::shared_ptr<ConnState>> connections_list_;
};

} // namespace server
