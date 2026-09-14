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
#include <vector>

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

// readline 版的行读取器：上下翻历史 / backspace / 行内编辑 / TAB 补全。
// 没有编入 readline 时返回空 -> REPL 自动退回 std::getline。
//
//   connection   上下文补全用（当前库的表名）；可为 nullptr
//   history_path 历史文件（空 = 不落盘）；创建时 load、析构时 save
LineReader readline_line_reader(SqlConnection *connection,
                                const std::string &history_path = {});

// 把内存里的 readline 历史写回文件（没有 readline 时是 no-op）。
// 交互循环返回后调用一次即可（Ctrl-D 那条路径内部已经写过）。
void history_flush(const std::string &history_path);

// 补全候选：元命令 + 关键字（来源 sql.l 的 lex_keywords()）+ 当前库的表名
// （需要 connection 支持元信息）。已经按当前输入过滤并排序去重。
// 单独暴露是为了能在没有 TTY / readline 的情况下单测。
std::vector<std::string> completion_candidates(const std::string &line,
                                               SqlConnection *connection);

// 跑一段文本（可能含多条语句与元命令行）；返回失败条数
int run_text(SqlConnection &connection, const std::string &text,
             const ReplOptions &options);

// 交互循环（输入 exit/quit/\q 结束）；返回进程退出码
int run(SqlConnection &connection, const ReplOptions &options,
        const LineReader &reader = {});

} // namespace client
