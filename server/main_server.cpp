// server/main_server.cpp —— sqldb-server 入口
//
//   sqldb-server [--config=path] [--listen=host:port] [-h|--help]
//
// 流程：读配置 → 打开存储（一份 KVStore）→ 建 Server（监听 + 服务线程）→
// 跑事件循环；SIGINT/SIGTERM 触发优雅退出（停 accept → 停服务线程 → 关存储）。
//
// 日志：级别由 `[server] log_level` 决定，去处由 `[server] log_file` 决定
// （空 = stderr）。两者都在这里**启动期**定下来，日志文件打不开直接退出，
// 免得服务跑起来了却"以为在记日志"。
#include <csignal>
#include <cstdio>
#include <string>

#include <fmt/format.h>

#include "config.h"
#include "logger.h"
#include "raft_bootstrap.h"
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

void print_usage(const char *program) {
  fmt::print("用法: {} [--config=PATH] [--listen=HOST:PORT]\n", program);
  fmt::print("\n选项:\n");
  fmt::print(
      "  --config=PATH        配置文件（.ini，见下）；不传则用内置默认值\n");
  fmt::print(
      "  --listen=HOST:PORT   覆盖配置里的 server.listen（临时起服务用）\n");
  fmt::print("  -h, --help           显示这份帮助\n");
  fmt::print("\n配置文件：[section] + key = value，'#'/';' 开头是注释，\n");
  fmt::print("未知 section 合法（留给别的组件）。带注释的**默认配置**在\n");
  fmt::print("  build/svr/etc/sqldb-server.conf\n");
  fmt::print("（构建时从 server/etc/sqldb-server.conf 复制过来）\n");
  fmt::print("\n日志:\n");
  fmt::print(
      "  [server] log_level = error|warn|info|debug（默认 info，是上限）\n");
  fmt::print("  [server] log_file  = 日志文件路径（空 = 写 stderr）\n");
  fmt::print("  行格式：[YYYY-MM-DD HH:MM:SS.mmm] [level] message\n");
  fmt::print(
      "  文件以 append 方式打开，每条日志一次 write() —— 追加是原子的。\n");
  fmt::print("\n例:\n");
  fmt::print("  {} --config=build/svr/etc/sqldb-server.conf\n", program);
  fmt::print("  # 想看 debug：配置里写 log_level = debug（或另存一份改）\n");
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
  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    if (arg == "-h" || arg == "--help") {
      print_usage(argv[0]);
      return 0;
    }
  }
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
    config->set("server.listen", listen);
  }
  // 语法之后再看字段：拼错的 key/类型不符/超范围都在这一步拦下
  if (auto ok = config->validate(); !ok.has_value()) {
    fmt::print(stderr, "配置错误: {}\n", ok.error());
    return 2;
  }

  // [raft] 的配置项（含校验）已经落地，但 P1b 的接线尚未完成：把存储切到
  // Raft 需要先有生产 transport 与 RaftRuntime 的启动/关闭顺序。这里**故意
  // 硬失败**，而不是静默继续跑本地存储 —— 否则用户会以为写已经被复制了。
  //
  // 接线时的顺序（见 raft/codex_glm53_validate_design.md 的生命周期一节）：
  //   logger -> 本地 KVStore -> Raft LogStore -> StateMachine/RequestResultStore
  //   -> Transport -> RaftRuntime -> RaftKVStore -> Server(raft_store)
  // 退出顺序反过来：停 accept -> 停 raft -> 落盘/关 LogStore -> 关本地 store。
  if (config->raft_enabled()) {
#if !defined(SQLDB_HAVE_LEVELDB)
    fmt::print(stderr, "配置错误: raft.enabled = true 需要带 leveldb 的构建\n");
    return 2;
#endif
  }

  // 日志接收端：level 非法 / 文件打不开都在这里拦下（启动期失败，别静默）。
  // 放在打开存储之前：Raft 的恢复/选主日志同样重要，而这些步骤紧跟着来。
  auto logger = server::Logger::create(config->log_level(), config->log_file());
  if (!logger.has_value()) {
    fmt::print(stderr, "日志初始化失败: {}\n", logger.error());
    return 2;
  }

  const std::string engine = config->engine();
  kv::DatabaseOptions options;
  options.set_path(config->path())
      .set_create_if_missing(config->create_if_missing())
      .set_error_if_exists(false);
  const kv::EngineType type =
      engine == "mock" ? kv::EngineType::MOCK : kv::EngineType::LEVELDB;
  auto store = kv::open_store(type, options);
  if (store == nullptr) {
    fmt::print(stderr, "打开存储失败: {} ({})\n", config->path(), engine);
    return 1;
  }

  // raft.enabled：本地 store 作为状态机，外面再包一层 Raft。
  // 顺序见 raft/codex_glm53_validate_design.md 的生命周期一节。
  std::shared_ptr<kv::KVStore> sql_store = store;
#if defined(SQLDB_HAVE_LEVELDB)
  std::unique_ptr<server::RaftBootstrap> raft;
  if (config->raft_enabled()) {
    auto created =
        server::RaftBootstrap::open(*config, store, **logger);
    if (!created.has_value()) {
      fmt::print(stderr, "启动 Raft 失败: {}\n", created.error());
      return 1;
    }
    raft = std::move(*created);
    sql_store = raft->store();
  }
#endif

  server::Server server(*config, sql_store, *logger);
  if (auto ok = server.listen(); !ok.has_value()) {
    fmt::print(stderr, "监听失败: {}\n", ok.error());
    return 1;
  }
  g_server = &server;
  std::signal(SIGINT, on_signal);
  std::signal(SIGTERM, on_signal);

  fmt::print(stderr, "sqldb-server 正在监听 {} (engine={}, path={})\n",
             config->listen(), engine, config->path());
  if (config->raft_enabled()) {
    fmt::print(stderr, "raft: node_id={} listen={} (group size={})\n",
               config->raft_node_id(), config->raft_listen(),
               config->raft_peers().size());
  }
  if (!config->log_file().empty()) {
    fmt::print(stderr, "日志级别 {} -> {}\n", config->log_level(),
               config->log_file());
  }
  server.run();

  // 收尾：连接都退出了才能关 raft/存储（还有活跃快照时 close() 会返回 Busy）
#if defined(SQLDB_HAVE_LEVELDB)
  if (raft != nullptr) {
    raft->stop();
  }
#endif
  const kv::Status closed = sql_store->close();
  if (closed != kv::Status::OK) {
    fmt::print(stderr, "关闭存储未完成: {}\n", kv::status_to_string(closed));
    return 1;
  }
  fmt::print(stderr, "sqldb-server 已退出\n");
  return 0;
}
