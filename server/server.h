// server/server.h
//
// sqldb 服务器应用层：协议帧 + Session 路由 + parse/write/read 服务线程。
// TCP listen/accept、连接所有权、优雅退出等通用生命周期在
// common::svrkit::TcpServer 中（Raft 等服务可复用同一套框架）。
//
//   svrkit::TcpServer(主线程)   ParseService   WriteService   ReadPool[N]
//     └─ connection 协程 ── SQL ──► co_await 提交 ──► ...
//            ▲                                      │
//            └──────────── Loop::post(resume) ◄─────┘
//
// 路由规则：`SELECT` 且不在事务里 → 读线程池；其余一切 → WriteService。
#pragma once

#include <atomic>
#include <cstdint>
#include <expected>
#include <memory>
#include <string>
#include <vector>

#include "common/svrkit/service.h"
#include "common/svrkit/tcp_server.h"
#include "config.h"
#include "logger.h"
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
  std::atomic<uint64_t> read_queue_rejected{0};  // 读队列满被拒的次数
};

class Server {
public:
  // logger 为 nullptr 时按配置自己建一个（建不出来就退回 stderr）；
  // 进程入口想对"日志文件打不开"报错，就自己 `Logger::create()` 再传进来。
  Server(ServerConfig config, std::shared_ptr<kv::KVStore> store,
         std::shared_ptr<Logger> logger = nullptr);
  ~Server();

  Server(const Server &) = delete;
  Server &operator=(const Server &) = delete;

  // 建监听 socket；失败返回信息
  std::expected<void, std::string> listen();
  // 跑事件循环（阻塞）；stop() 可以从别的线程调
  void run();
  // 立刻停（测试/异常路径用）：不等连接协议收尾
  void stop() { transport_.stop(); }

  // 优雅退出：停止接受新连接 -> 通知所有连接收尾 -> 等它们退出（有上限）
  // -> 停事件循环。可以从信号处理里安全调用。
  void request_shutdown() { transport_.request_shutdown(); }

  // 把一条已经建立的连接挂到事件循环上（socketpair 测试/外部 accept）。
  void attach_connection(int fd) { transport_.attach_connection(fd); }

  const Metrics &metrics() const { return metrics_; }
  bool shutting_down() const { return transport_.shutting_down(); }

  int port() const { return transport_.port(); }
  kv::KVStore &store() { return *store_; }
  const ServerConfig &config() const { return config_; }
  common::svrkit::Loop &loop() { return transport_.loop(); }
  common::svrkit::ServiceThread &parse_service() { return parse_service_; }
  common::svrkit::ServiceThread &write_service() { return write_service_; }
  size_t read_pool_size() const { return read_pool_.size(); }
  size_t connection_count() const { return transport_.connection_count(); }

private:
  // SQL 层的空闲看门狗状态：定时器回调与连接协程共享
  struct ConnState {
    std::shared_ptr<common::svrkit::TcpConnection> connection;
    std::atomic<bool> timed_out{false};
    std::atomic<uint64_t> generation{0}; // 每条语句 +1，让旧定时器失效
  };

  // 一条连接：HELLO → 循环(读帧 → 处理 → 写响应) → BYE。
  // 返回后由 TcpServer 统一关闭 fd 并注销连接。
  common::svrkit::Task
  serve_connection(std::shared_ptr<common::svrkit::TcpConnection> connection);

  void log(const char *level, const std::string &message) const;
  static common::svrkit::TcpServerOptions
  make_transport_options(const ServerConfig &config, Server *owner);
  // 空闲看门狗：到点只关读方向（协程醒来后还能回 ERROR 帧）
  void arm_idle_watchdog(const std::shared_ptr<ConnState> &state, bool in_tx);

  ServerConfig config_;
  std::shared_ptr<kv::KVStore> store_;
  std::shared_ptr<Logger> logger_;
  common::svrkit::TcpServer transport_;
  common::svrkit::ServiceThread parse_service_;
  common::svrkit::ServiceThread write_service_;
  std::vector<std::unique_ptr<common::svrkit::ServiceThread>> read_pool_;
  std::atomic<uint64_t> read_next_{0}; // 读池轮询计数（无锁取模）
  Metrics metrics_;
};

} // namespace server
