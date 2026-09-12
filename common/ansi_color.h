#ifndef COMMON_ANSI_COLOR_H
#define COMMON_ANSI_COLOR_H

// ANSI 颜色工具（C++ 侧用；纯头文件，无依赖）
//
// 约定：
//   - 库本身不决定"要不要上色"的最终策略，由调用方传 enabled；
//   - ansi::enabled_by_default() 给 CLI 一个合理默认：
//     设置了 NO_COLOR、或 stdout 不是终端时，返回 false。

#include <cstdio>
#include <cstdlib>
#include <string>
#include <string_view>

#if defined(__unix__) || defined(__APPLE__)
#include <unistd.h>
#endif

namespace sql {
namespace ansi {

inline constexpr std::string_view kReset = "\x1b[0m";
inline constexpr std::string_view kBold = "\x1b[1m";
inline constexpr std::string_view kDim = "\x1b[2m";
inline constexpr std::string_view kRed = "\x1b[31m";
inline constexpr std::string_view kGreen = "\x1b[32m";
inline constexpr std::string_view kYellow = "\x1b[33m";
inline constexpr std::string_view kBlue = "\x1b[34m";
inline constexpr std::string_view kMagenta = "\x1b[35m";
inline constexpr std::string_view kCyan = "\x1b[36m";
inline constexpr std::string_view kBrightRed = "\x1b[91m";
// 组合样式
inline constexpr std::string_view kKeyword = "\x1b[1;34m";   // 粗体蓝
inline constexpr std::string_view kErrorSpan = "\x1b[1;91m"; // 粗体亮红

// stdout 是不是终端 + 有没有 NO_COLOR
inline bool enabled_by_default() {
  if (std::getenv("NO_COLOR") != nullptr) {
    return false;
  }
#if defined(__unix__) || defined(__APPLE__)
  return ::isatty(::fileno(stdout)) != 0;
#else
  return false;
#endif
}

inline std::string paint(std::string_view text, std::string_view code,
                         bool enabled) {
  if (!enabled || code.empty()) {
    return std::string(text);
  }
  std::string out;
  out.reserve(text.size() + code.size() + kReset.size());
  out.append(code);
  out.append(text);
  out.append(kReset);
  return out;
}

} // namespace ansi
} // namespace sql

#endif // COMMON_ANSI_COLOR_H
