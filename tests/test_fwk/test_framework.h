// tests/fwk/test_framework.h
#pragma once

#include <chrono>
#include <fmt/format.h>
#include <functional>
#include <iostream>
#include <source_location>
#include <sstream>
#include <string>
#include <vector>

namespace test {

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
// 全局统计
// ============================================================
inline int failures = 0;
inline int assertions = 0;

// ============================================================
// 格式化辅助
// ============================================================
template <typename T>
concept Streamable = requires(const T& t, std::ostream& os) { os << t; };

template <typename T>
std::string fmt(const T& v) {
    if constexpr (std::is_same_v<T, bool>) {
        return v ? "true" : "false";
    } else if constexpr (std::is_same_v<T, std::string>) {
        return v;
    } else if constexpr (Streamable<T>) {
        return fmt::format("{}", v);
    } else if constexpr (std::is_arithmetic_v<T>) {
        return fmt::format("{}", v);
    } else {
        return "<unprintable>";
    }
}

// ============================================================
// 断言失败报告
// ============================================================
inline void report_failure(const char* expr_str, const std::string& message,
                           const std::source_location& loc) {
    failures++;
    fmt::print(stderr, "[FAILED] {}:{}:{}: {}\n", 
               loc.file_name(), loc.line(), loc.column(), message);
    if (expr_str) {
        fmt::print(stderr, "  Expression: {}\n", expr_str);
    }
}

// ============================================================
// run_all：运行所有测试（inline 定义在头文件）
// ============================================================
inline int run_all() {
    auto& tests = registry();
    
    fmt::print("Running {} tests in {} suites...\n\n", 
               tests.size(), 0);
    
    std::string current_suite;
    int suite_tests = 0;
    int suite_failures = 0;
    
    auto start_time = std::chrono::steady_clock::now();
    
    for (auto& t : tests) {
        if (t.suite != current_suite) {
            if (!current_suite.empty()) {
                fmt::print("\n[SUITE] {}: {} tests, {} failures\n", 
                           current_suite, suite_tests, suite_failures);
            }
            current_suite = t.suite;
            suite_tests = 0;
            suite_failures = 0;
            fmt::print("\n===== {} =====\n", t.suite);
        }
        
        int before = failures;
        auto test_start = std::chrono::steady_clock::now();
        
        fmt::print("[ RUN  ] {}\n", t.name);
        t.fn();
        
        auto test_end = std::chrono::steady_clock::now();
        auto us = std::chrono::duration_cast<std::chrono::microseconds>(test_end - test_start).count();
        
        suite_tests++;
        if (failures == before) {
            fmt::print("[  OK  ] {} ({:.2f} ms)\n", t.name, us / 1000.0);
        } else {
            auto count = failures - before;
            suite_failures += count;
            fmt::print("[FAILED] {} ({} failures, {:.2f} ms)\n", 
                       t.name, count, us / 1000.0);
        }
    }
    
    if (!current_suite.empty()) {
        fmt::print("\n[SUITE] {}: {} tests, {} failures\n", 
                   current_suite, suite_tests, suite_failures);
    }
    
    auto end_time = std::chrono::steady_clock::now();
    auto total_ms = std::chrono::duration_cast<std::chrono::milliseconds>(end_time - start_time).count();
    
    fmt::print("\n========================================\n");
    fmt::print("Total: {} tests, {} assertions, {} failures ({:.3f} s)\n", 
               tests.size(), assertions, failures, total_ms / 1000.0);
    
    return failures > 0 ? 1 : 0;
}

}  // namespace test

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

#define CHECK_THROW(expr) \
    do { \
        test::assertions++; \
        bool caught = false; \
        try { expr; } catch (...) { caught = true; } \
        if (!caught) { \
            test::report_failure(#expr, fmt::format("Expected exception, but none thrown"), \
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