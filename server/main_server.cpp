// server/main_server.cpp —— sqldb-server 入口
//
//   sqldb-server [--config=path] [--listen=host:port]
//
// 流程：读配置 → 打开存储（一份 KVStore）→ 建 Server（监听 + 服务线程）→
// 跑事件循环；SIGINT/SIGTERM 触发优雅退出（停 accept → 停服务线程 → 关存储）。
#include <csignal>
#include <cstdio>
#include <string>

#include <fmt/format.h>

#include "config.h"
#include "server.h"
#include "storage/kv_engine/kv_factory.h"

namespace {

server::Server *g_server = nullptr;

void on_signal(int) {
  if (g_server != nullptr) {
    // 只 write 一根管道：异步信号安全；真正的收尾在事件循环里做
    g_server->request_shutdown();
  }
}

std::string arg_value(int argc, char **argv, const std::string &name,
                      const std::string &fallback) {
  const std::string prefix = name + "=";
  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    if (arg.rfind(prefix, 0) == 0) {
      return arg.substr(prefix.size());
    }
  }
  return fallback;
}

} // namespace

int main(int argc, char **argv) {
  const std::string config_path = arg_value(argc, argv, "--config", "");
  auto config = config_path.empty() ? server::parse_config(std::string())
                                    : server::load_config(config_path);
  if (!config.has_value()) {
    fmt::print(stderr, "配置错误: {}\n", config.error());
    return 2;
  }
  // 命令行覆盖（方便测试/临时起服务）
  const std::string listen = arg_value(argc, argv, "--listen", "");
  if (!listen.empty()) {
    config->listen = listen;
    const size_t colon = listen.rfind(':');
    if (colon == std::string::npos) {
      fmt::print(stderr, "配置错误: --listen 需要 host:port\n");
      return 2;
    }
    config->host = listen.substr(0, colon);
    config->port = listen.substr(colon + 1);
  }

  kv::DatabaseOptions options;
  options.set_path(config->path)
      .set_create_if_missing(config->create_if_missing)
      .set_error_if_exists(false);
  const kv::EngineType type =
      config->engine == "mock" ? kv::EngineType::MOCK : kv::EngineType::LEVELDB;
  auto store = kv::open_store(type, options);
  if (store == nullptr) {
    fmt::print(stderr, "打开存储失败: {} ({})\n", config->path, config->engine);
    return 1;
  }

  server::Server server(*config, store);
  if (auto ok = server.listen(); !ok.has_value()) {
    fmt::print(stderr, "监听失败: {}\n", ok.error());
    return 1;
  }
  g_server = &server;
  std::signal(SIGINT, on_signal);
  std::signal(SIGTERM, on_signal);

  fmt::print(stderr, "sqldb-server 正在监听 {} (engine={}, path={})\n",
             config->listen, config->engine, config->path);
  server.run();

  // 收尾：连接都退出了才能关存储（还有活跃快照时 close() 会返回 Busy）
  const kv::Status closed = store->close();
  if (closed != kv::Status::OK) {
    fmt::print(stderr, "关闭存储未完成: {}\n", kv::status_to_string(closed));
    return 1;
  }
  fmt::print(stderr, "sqldb-server 已退出\n");
  return 0;
}
