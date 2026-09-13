// server/config.h
//
// 配置文件（INI 子集）：`[section]` + `key = value`，`#`/`;` 注释。
// 解析失败**必须启动失败**（带行号），配置错误不能靠默认值蒙混过去。
//
// 设计约定：库代码不打印 —— 这里只返回错误信息，由 main 决定怎么报。
#pragma once

#include <cstdint>
#include <expected>
#include <string>

namespace server {

struct Config {
  // [server]
  std::string listen = "127.0.0.1:5433";
  size_t max_connections = 256;
  int64_t idle_timeout_ms = 300000; // 0 = 不超时
  int64_t idle_in_transaction_timeout_ms =
      30000;                      // 0 = 不超时（钉快照的危险项）
  std::string log_level = "info"; // error | warn | info | debug

  // [storage]
  std::string engine = "leveldb"; // leveldb | mock（mock 供测试/开发）
  std::string path = "./sql_db";
  bool create_if_missing = true;

  // [execution]
  size_t read_threads = 1; // 只读协程跑在几条线程上（M1 固定 1，M3 可调大）
  size_t write_queue_max = 1024;
  size_t max_result_rows = 1000000;
  int64_t statement_timeout_ms = 0; // 0 = 不限（M3 有协作检查点后才能真正打断）

  // [session]
  std::string default_database; // 新连接自动 USE（空 = 不 USE）

  // 派生字段（从 listen 拆出来）
  std::string host;
  std::string port;
};

// 解析配置文本；失败返回"line N: ..."形式的信息
std::expected<Config, std::string> parse_config(const std::string &text);
// 从文件读入并解析
std::expected<Config, std::string> load_config(const std::string &path);

} // namespace server
