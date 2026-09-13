// server/config.h
//
// 两层结构：
//   1) Config —— 通用配置包：`.ini`（`[section]` + `key = value`，`#`/`;`
//      注释）解析成 map<section, map<key, sql::Value>>。解析器**不认识任何
//      具体字段**，只做语法 + 类型嗅探；别的组件也可以往同一个文件里放
//      自己的 section（未知 section 合法）。
//   2) ServerConfig —— 服务器认识的字段集合，**每个字段一个方法**
//      （`listen()` / `read_threads()` / …）。调用点只看这些方法，不碰
//      dotted key、不碰取值宏 —— 宏只活在 config.cpp 内部（ServerConfig
//      的实现靠它从通用包里取值）。
//
// 三层防线：
//   1) 语法错（缺 `]`、缺 `=`）→ parse_config 失败，带行号；
//   2) 字段错（已知 section 里拼错的 key、类型不符、超范围）→ validate()
//      失败，带字段名 —— 内置默认值表同时充当"服务器认识哪些 key、
//      各 key 是什么类型"的 schema；
//   3) 取值时类型不符 → 抛异常（validate 过了就不该发生，发生了是 bug）。
//
// 未知 **section** 合法（别的组件的自留地）；已知 section 里的未知 **key**
// 会被 validate() 拦下（多半是拼错了）。
//
// 启动路径：parse_config/load_config → 命令行覆盖（set）→ validate() →
// 用 ServerConfig 的方法取值。
#pragma once

#include <cstddef>
#include <cstdint>
#include <expected>
#include <map>
#include <string>
#include <string_view>

#include "sql_types/value.h"

namespace server {

class Config {
public:
  // std::less<> = 透明比较：find(string_view) 不用先造 string
  using Section = std::map<std::string, sql::Value, std::less<>>;
  using Table = std::map<std::string, Section, std::less<>>;

  Config(); // 带上内置默认值（默认值表 = 服务器认识的字段集合 + 类型 schema）

  // ----- 取值（dotted = "section.key"；没有点 = 全局键）-----
  // 键不存在 → fallback；存在但类型不符 → std::runtime_error
  std::string get_string(std::string_view dotted,
                         std::string fallback = "") const;
  int64_t get_int(std::string_view dotted, int64_t fallback = 0) const;
  bool get_bool(std::string_view dotted, bool fallback = false) const;
  bool contains(std::string_view dotted) const {
    return find(dotted) != nullptr;
  }
  const sql::Value *find(std::string_view dotted) const;

  // 写入/覆盖（测试、命令行覆盖用）
  void set(std::string_view dotted, sql::Value value);
  // overlay 覆盖本表同 section.key 的项
  void merge(const Table &overlay);

  // 启动期校验：已知 section 的 key/类型对默认值表，再查范围/枚举
  std::expected<void, std::string> validate() const;

  const Table &table() const { return table_; }

private:
  Table table_;
};

// 服务器字段的类型化门面：每个字段一个方法，调用点不碰 dotted key 与宏。
class ServerConfig {
public:
  ServerConfig(); // 带内置默认值
  explicit ServerConfig(Config generic);

  // 写入/覆盖（测试、命令行覆盖用）：dotted = "section.key"
  void set(std::string_view dotted, sql::Value value);
  // 启动期校验（同 Config::validate）
  std::expected<void, std::string> validate() const;
  // 通用包本体（别的组件取自己 section 的逃生舱）
  const Config &generic() const { return generic_; }

  // [server]
  std::string listen() const;
  size_t max_connections() const;
  int64_t idle_timeout_ms() const; // 0 = 不超时
  // 0 = 不超时（事务挂着不动会把 leveldb 旧版本钉住，危险项）
  int64_t idle_in_transaction_timeout_ms() const;
  std::string log_level() const; // error | warn | info | debug
  // server.listen 的派生量（listen 非法时返回空 —— validate 会先拦下）
  std::string listen_host() const;
  std::string listen_port() const;

  // [storage]
  std::string engine() const; // leveldb | mock（mock 供测试/开发）
  std::string path() const;
  bool create_if_missing() const;

  // [execution]
  size_t read_threads() const;     // 只读协程跑在几条线程上
  size_t read_queue_max() const;   // 读服务队列上限
  size_t write_queue_max() const;  // 写/解析服务队列上限
  size_t max_result_rows() const;  // 单条语句最多回多少行
  int64_t statement_timeout_ms() const; // 0 = 不限（要 M3 协作检查点才能真打断）

  // [session]
  std::string default_database() const; // 新连接自动 USE（空 = 不 USE）

private:
  Config generic_;
};

// 解析配置文本（只做语法 + 类型嗅探；字段校验见 validate()）。
// 返回的 ServerConfig 已带内置默认值，text 里的项覆盖默认值。
std::expected<ServerConfig, std::string> parse_config(const std::string &text);
// 从文件读入并解析
std::expected<ServerConfig, std::string> load_config(const std::string &path);

} // namespace server
