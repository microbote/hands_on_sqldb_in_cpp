// tests/fwk/test_framework.h
#pragma once

#include <chrono>
#include <cstdlib>
#include <fmt/format.h>
#include <functional>
#include <iostream>
#include <source_location>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>
#include <algorithm>

namespace test {

// ============================================================
// 颜色控制（自动检测终端支持，可通过 --no-color 禁用）
// ============================================================
namespace color {

inline bool& enabled() {
    static bool e = [] {
        const char* no_color = std::getenv("NO_COLOR");
        if (no_color && no_color[0] != '\0') { return false;
}
        const char* term = std::getenv("TERM");
        if (!term) { return false;
}
        return std::string_view(term) != "dumb";
    }();
    return e;
}

inline void set_enabled(bool e) { enabled() = e; }

inline std::string_view reset()   { return enabled() ? "\033[0m"  : ""; }
inline std::string_view red()     { return enabled() ? "\033[31m" : ""; }
inline std::string_view green()   { return enabled() ? "\033[32m" : ""; }
inline std::string_view yellow()  { return enabled() ? "\033[33m" : ""; }
inline std::string_view blue()    { return enabled() ? "\033[34m" : ""; }
inline std::string_view magenta(){ return enabled() ? "\033[35m" : ""; }
inline std::string_view cyan()    { return enabled() ? "\033[36m" : ""; }
inline std::string_view bold()    { return enabled() ? "\033[1m"  : ""; }

} // namespace color

// ============================================================
// 测试用例注册表
// ============================================================
struct TestCase {
    std::string name;
    std::string suite;
    std::function<void()> fn;
};

inline std::vector<TestCase>& registry() {
    static std::vector<TestCase> tests;
    return tests;
}

struct TestRegistrar {
    TestRegistrar(std::string suite, std::string name, std::function<void()> fn) {
        registry().push_back({std::move(name), std::move(suite), std::move(fn)});
    }
};

// ============================================================
// 全局统计与配置
// ============================================================
inline int failures = 0;
inline int assertions = 0;
inline bool break_on_failure = false;

// ============================================================
// 格式化辅助（支持指针、nullptr、string_view 等）
// ============================================================
template <typename T>
concept Streamable = requires(const T& t, std::ostream& os) { os << t; };

template <typename T>
std::string fmt(const T& v) {
    using U = std::decay_t<T>;
    if constexpr (std::is_same_v<U, bool>) {
        return v ? "true" : "false";
    } else if constexpr (std::is_null_pointer_v<U>) {
        return "nullptr";
    } else if constexpr (std::is_pointer_v<U>) {
        if (v == nullptr) { return "nullptr";
}
        return fmt::format("{}", static_cast<const void*>(v));
    } else if constexpr (std::is_same_v<U, std::string> || std::is_same_v<U, std::string_view>) {
        return std::string(v);
    } else if constexpr (std::is_same_v<U, const char*>) {
        return v ? std::string(v) : "nullptr";
    } else if constexpr (Streamable<T>) {
        return fmt::format("{}", v);
    } else if constexpr (std::is_arithmetic_v<T>) {
        return fmt::format("{}", v);
    } else {
        return "<unprintable>";
    }
}

// ============================================================
// 断言失败报告（带颜色）
// ============================================================
inline void report_failure(const char* expr_str, const std::string& message,
                           const std::source_location& loc) {
    failures++;
    fmt::print(stderr, "{}[FAILED]{} {}:{}:{}: {}\n",
               color::red(), color::reset(),
               loc.file_name(), loc.line(), loc.column(), message);
    if (expr_str != nullptr) {
        fmt::print(stderr, "  {}Expression:{} {}\n",
                   color::yellow(), color::reset(), expr_str);
    }
    if (break_on_failure) {
        std::abort();
    }
}

// ============================================================
// 运行配置
// ============================================================
struct RunConfig {
    std::string filter;          // 匹配 "SuiteName" 或 "SuiteName.TestName"
    bool list_only = false;
    bool no_color = false;
};

inline bool name_matches(const std::string& suite, const std::string& name,
                         const std::string& filter) {
    if (filter.empty()) { return true;
}
    auto full = suite + "." + name;
    if (full.find(filter) != std::string::npos) { return true;
}
    if (suite.find(filter) != std::string::npos) { return true;
}
    return false;
}

// ============================================================
// run_all：运行所有测试（异常安全、颜色、过滤）
// ============================================================
inline int run_all(const RunConfig& config = {}) {
    if (config.no_color) { color::set_enabled(false);
}

    // 每次运行独立统计
    failures = 0;
    assertions = 0;

    auto& tests = registry();

    // 统计 suite 数量
    std::unordered_set<std::string> suites;
    for (auto& t : tests) { suites.insert(t.suite);
}

    // 按 filter 过滤
    std::vector<TestCase*> filtered;
    for (auto& t : tests) {
        if (name_matches(t.suite, t.name, config.filter)) {
            filtered.push_back(&t);
        }
    }

    if (config.list_only) {
        for (auto* t : filtered) {
            fmt::print("{}.{}{}\n", t->suite, color::cyan(), t->name, color::reset());
        }
        return 0;
    }

    fmt::print("Running {} tests from {} suites ({} total registered)...\n\n",
               filtered.size(), suites.size(), tests.size());

    if (filtered.empty()) {
        fmt::print("{}No tests matched the filter.{}\n",
                   color::yellow(), color::reset());
        return 0;
    }

    std::string current_suite;
    int suite_tests = 0;
    int suite_failures = 0;

    auto start_time = std::chrono::steady_clock::now();

    for (auto* t : filtered) {
        if (t->suite != current_suite) {
            if (!current_suite.empty()) {
                auto sc = suite_failures > 0 ? color::red() : color::green();
                fmt::print("\n[SUITE] {}{}{}: {} tests, {} failures\n",
                           sc, current_suite, color::reset(), suite_tests, suite_failures);
            }
            current_suite = t->suite;
            suite_tests = 0;
            suite_failures = 0;
            fmt::print("\n{}===== {} ====={}\n",
                       color::bold(), t->suite, color::reset());
        }

        int before = failures;
        auto test_start = std::chrono::steady_clock::now();

        fmt::print("[ {}RUN{} ] {}.{}\n",
                   color::blue(), color::reset(), t->suite, t->name);

        // 异常安全：捕获所有异常，避免一个测试崩溃导致后续全部中断
        try {
            t->fn();
        } catch (const std::exception& e) {
            report_failure(nullptr,
                           fmt::format("Unhandled exception: {}", e.what()),
                           std::source_location::current());
        } catch (...) {
            report_failure(nullptr, "Unhandled unknown exception",
                           std::source_location::current());
        }

        auto test_end = std::chrono::steady_clock::now();
        auto us = std::chrono::duration_cast<std::chrono::microseconds>(test_end - test_start).count();

        suite_tests++;
        if (failures == before) {
            fmt::print("[  {}{}OK{}{}  ] {}.{} ({:.2f} ms)\n",
                       color::green(), color::bold(), color::reset(), color::green(),
                       t->suite, t->name, us / 1000.0);
        } else {
            auto count = failures - before;
            suite_failures += count;
            fmt::print("[ {}{}FAIL{}{} ] {}.{} ({} failures, {:.2f} ms)\n",
                       color::red(), color::bold(), color::reset(), color::red(),
                       t->suite, t->name, count, us / 1000.0);
        }
    }

    if (!current_suite.empty()) {
        auto sc = suite_failures > 0 ? color::red() : color::green();
        fmt::print("\n[SUITE] {}{}{}: {} tests, {} failures\n",
                   sc, current_suite, color::reset(), suite_tests, suite_failures);
    }

    auto end_time = std::chrono::steady_clock::now();
    auto total_ms = std::chrono::duration_cast<std::chrono::milliseconds>(end_time - start_time).count();

    auto total_color = failures > 0 ? color::red() : color::green();
    fmt::print("\n{}========================================{}\n",
               color::bold(), color::reset());
    fmt::print("{}Total:{} {} tests, {} assertions, {} failures ({:.3f} s)\n",
               total_color, color::reset(),
               filtered.size(), assertions, failures, total_ms / 1000.0);

    return failures > 0 ? 1 : 0;
}

// 解析 argc/argv 的便捷入口
inline int run_all(int argc, char** argv) {
    RunConfig config;
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--list" || arg == "-l") {
            config.list_only = true;
        } else if (arg == "--no-color") {
            config.no_color = true;
        } else if (arg == "--break-on-failure") {
            break_on_failure = true;
        } else if (arg.starts_with("--filter=")) {
            config.filter = arg.substr(9);
        } else if (arg == "--help" || arg == "-h") {
            fmt::print("Usage: {} [options]\n", argv[0]);
            fmt::print("Options:\n");
            fmt::print("  -l, --list             List all registered tests\n");
            fmt::print("  --filter=PATTERN       Run only tests matching pattern\n");
            fmt::print("  --no-color             Disable colored output\n");
            fmt::print("  --break-on-failure     Abort on first assertion failure\n");
            fmt::print("  -h, --help             Show this help\n");
            return 0;
        }
    }
    return run_all(config);
}

} // namespace test

// ============================================================
// 断言宏
// ============================================================
#define CHECK(expr) \
    do { \
        test::assertions++; \
        if (!(expr)) { \
            test::report_failure(#expr, fmt::format("CHECK failed: {}", #expr), \
                                std::source_location::current()); \
        } \
    } while(0)

#define CHECK_TRUE(expr)  CHECK(expr)
#define CHECK_FALSE(expr) CHECK(!(expr))

#define CHECK_EQ(a, b) \
    do { \
        test::assertions++; \
        if (!((a) == (b))) { \
            test::report_failure(#a " == " #b, fmt::format("{} != {}", test::fmt(a), test::fmt(b)), \
                                std::source_location::current()); \
        } \
    } while(0)

#define CHECK_NE(a, b) \
    do { \
        test::assertions++; \
        if (!((a) != (b))) { \
            test::report_failure(#a " != " #b, fmt::format("{} == {}", test::fmt(a), test::fmt(b)), \
                                std::source_location::current()); \
        } \
    } while(0)

#define CHECK_LT(a, b) \
    do { \
        test::assertions++; \
        if (!((a) < (b))) { \
            test::report_failure(#a " < " #b, fmt::format("{} >= {}", test::fmt(a), test::fmt(b)), \
                                std::source_location::current()); \
        } \
    } while(0)

#define CHECK_GT(a, b) \
    do { \
        test::assertions++; \
        if (!((a) > (b))) { \
            test::report_failure(#a " > " #b, fmt::format("{} <= {}", test::fmt(a), test::fmt(b)), \
                                std::source_location::current()); \
        } \
    } while(0)

#define CHECK_LE(a, b) \
    do { \
        test::assertions++; \
        if (!((a) <= (b))) { \
            test::report_failure(#a " <= " #b, fmt::format("{} > {}", test::fmt(a), test::fmt(b)), \
                                std::source_location::current()); \
        } \
    } while(0)

#define CHECK_GE(a, b) \
    do { \
        test::assertions++; \
        if (!((a) >= (b))) { \
            test::report_failure(#a " >= " #b, fmt::format("{} < {}", test::fmt(a), test::fmt(b)), \
                                std::source_location::current()); \
        } \
    } while(0)

#define CHECK_NULL(ptr) \
    do { \
        test::assertions++; \
        if ((ptr) != nullptr) { \
            test::report_failure(#ptr " == nullptr", fmt::format("{} != nullptr", test::fmt(ptr)), \
                                std::source_location::current()); \
        } \
    } while(0)

#define CHECK_NOT_NULL(ptr) \
    do { \
        test::assertions++; \
        if ((ptr) == nullptr) { \
            test::report_failure(#ptr " != nullptr", fmt::format("{} == nullptr", test::fmt(ptr)), \
                                std::source_location::current()); \
        } \
    } while(0)

#define CHECK_THROW(expr) \
    do { \
        test::assertions++; \
        bool caught = false; \
        try { expr; } catch (...) { caught = true; } \
        if (!caught) { \
            test::report_failure(#expr, "Expected exception, but none thrown", \
                                std::source_location::current()); \
        } \
    } while(0)

#define CHECK_THROW_AS(expr, ExceptType) \
    do { \
        test::assertions++; \
        bool caught = false; \
        try { expr; } \
        catch (const ExceptType&) { caught = true; } \
        catch (const std::exception& e) { \
            test::report_failure(#expr, \
                fmt::format("Expected " #ExceptType ", but got: {}", e.what()), \
                std::source_location::current()); \
        } catch (...) { \
            test::report_failure(#expr, "Expected " #ExceptType ", but got unknown exception", \
                                std::source_location::current()); \
        } \
        if (!caught) { \
            test::report_failure(#expr, "Expected " #ExceptType ", but none thrown", \
                                std::source_location::current()); \
        } \
    } while(0)

#define CHECK_NEAR(a, b, eps) \
    do { \
        test::assertions++; \
        auto _a = (a); auto _b = (b); \
        auto _diff = (_a > _b) ? (_a - _b) : (_b - _a); \
        if (_diff > (eps)) { \
            test::report_failure("|"#a" - "#b"| <= "#eps, \
                fmt::format("{} vs {} (diff = {})", test::fmt(_a), test::fmt(_b), _diff), \
                std::source_location::current()); \
        } \
    } while(0)

#define CHECK_STREQ(a, b) \
    do { \
        test::assertions++; \
        std::string_view _a(a); std::string_view _b(b); \
        if (_a != _b) { \
            test::report_failure(#a " == " #b, \
                fmt::format("\"{}\" != \"{}\"", _a, _b), \
                std::source_location::current()); \
        } \
    } while(0)

// ============================================================
// 测试定义宏
// ============================================================
#define TEST(SuiteName, TestName) \
    static void test_##SuiteName##_##TestName(); \
    static test::TestRegistrar reg_##SuiteName##_##TestName( \
        #SuiteName, #TestName, test_##SuiteName##_##TestName); \
    static void test_##SuiteName##_##TestName()