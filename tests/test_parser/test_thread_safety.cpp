// tests/test_parser/test_thread_safety.cpp
//
// **P0：多线程下的解析隔离**。
//
// 词法/语法层用的是进程级全局状态（flex 的扫描缓冲、yylloc/yylineno、
// ast.cpp 的 g_parsed_ast、parser.cpp 的 g_parser_state），所以
// `parser::Parser::parse()` 必须在内部串行化 —— 服务端会有多个线程同时
// 解析不同连接的语句。
//
// 这两个用例就是那把锁的守卫：去掉锁以后它们会偶发失败/崩溃。
#include "test_framework.h"

#include "parser/parser.h"

#include <atomic>
#include <string>
#include <thread>
#include <vector>

namespace {

std::string select_sql(int thread_id, int i) {
  return "SELECT id, name FROM t" + std::to_string(thread_id) +
         " WHERE id = " + std::to_string(i) + ";";
}

} // namespace

TEST(ParserThreadSafety, ConcurrentParsesReturnTheirOwnAst) {
  constexpr int kThreads = 8;
  constexpr int kIterations = 300;

  std::atomic<int> failures{0};
  std::vector<std::thread> threads;
  threads.reserve(kThreads);
  for (int t = 0; t < kThreads; ++t) {
    threads.emplace_back([t, &failures] {
      for (int i = 0; i < kIterations; ++i) {
        parser::Parser parser;
        const std::string sql = select_sql(t, i);
        auto result = parser.parse(sql);
        if (!result.success || result.ast == nullptr ||
            result.ast->type != NODE_SELECT) {
          ++failures;
          continue;
        }
        const auto *select =
            reinterpret_cast<const SelectNode *>(result.ast->data);
        const std::string expected = "t" + std::to_string(t);
        if (select->table == nullptr ||
            std::string(select->table) != expected) {
          ++failures; // 拿到了别人的 AST / 串了状态
        }
      }
    });
  }
  for (auto &thread : threads) {
    thread.join();
  }
  CHECK_EQ(failures.load(), 0);
}

TEST(ParserThreadSafety, ConcurrentErrorsDoNotBreakOtherThreads) {
  // 一半线程一直解析**坏语句**、一半一直解析好语句：
  // 好语句永远不能因为别人出错而失败（错误状态是每次解析独立的）。
  constexpr int kThreads = 8;
  constexpr int kIterations = 300;

  std::atomic<int> failures{0};
  std::vector<std::thread> threads;
  threads.reserve(kThreads);
  for (int t = 0; t < kThreads; ++t) {
    const bool bad = (t % 2) == 0;
    threads.emplace_back([t, bad, &failures] {
      for (int i = 0; i < kIterations; ++i) {
        parser::Parser parser;
        if (bad) {
          // "FROM" 后面缺表名：一定会失败
          auto result = parser.parse("SELECT id FROM WHERE id = 1;");
          if (result.success || !result.error.has_value() ||
              result.error->message.empty()) {
            ++failures;
          }
        } else {
          auto result = parser.parse(select_sql(t, i));
          if (!result.success || result.ast == nullptr) {
            ++failures; // 别人的语法错误串过来了
            continue;
          }
          const auto *select =
              reinterpret_cast<const SelectNode *>(result.ast->data);
          if (select->table == nullptr ||
              std::string(select->table) != "t" + std::to_string(t)) {
            ++failures;
          }
        }
      }
    });
  }
  for (auto &thread : threads) {
    thread.join();
  }
  CHECK_EQ(failures.load(), 0);
}
