// client/repl.h
//
// 共用 REPL：本地 `sqldb`（LocalConnection）与远程 `sqldb-client`
// （RemoteConnection）跑的是这里同一套代码 —— 语句切分、元命令、表格输出、
// 错误高亮（caret 在客户端渲染，服务端只回 span）。
#pragma once

#include <cstdio>
#include <functional>
#include <optional>
#include <string>

#include "connection.h"

namespace client {

struct ReplOptions {
  bool colors = false;
  bool echo_sql = false;
  bool interactive = false; // 交互模式下打印提示符与欢迎语
  FILE *out = stdout;
  FILE *err = stderr;
};

// 逐行读取（cmdline 用它接 readline；不传就从 stdin 读）
using LineReader =
    std::function<std::optional<std::string>(const std::string &prompt)>;

// 跑一段文本（可能含多条语句与元命令行）；返回失败条数
int run_text(SqlConnection &connection, const std::string &text,
             const ReplOptions &options);

// 交互循环（输入 exit/quit/\q 结束）；返回进程退出码
int run(SqlConnection &connection, const ReplOptions &options,
        const LineReader &reader = {});

} // namespace client
