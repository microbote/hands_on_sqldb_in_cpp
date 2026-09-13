// tests/test_executor/test_text_result.cpp
//
// exec::text_result()：把内存里的若干行文本变成"单列结果集"游标。
// EXPLAIN 用它把计划文本当普通结果集交给客户端（列名 "QUERY PLAN"），
// 于是 EXPLAIN 和别的语句共用 next()/close()/columns() 这一套。
#include "test_framework.h"

#include <memory>
#include <string>
#include <vector>

#include "executor/executor.h"

TEST(TextResult, YieldsOneRowPerLineAndEndsNormally) {
  auto cursor = exec::text_result(
      "QUERY PLAN", std::vector<std::string>{"FullScan(t)", "  Filter(x)"});
  CHECK(cursor != nullptr);
  if (cursor == nullptr) {
    return;
  }

  // 列名/形状：单列、不是写语句
  CHECK_EQ(cursor->columns().size(), size_t{1});
  if (cursor->columns().size() == 1) {
    CHECK_EQ(cursor->columns()[0], std::string("QUERY PLAN"));
  }
  CHECK(cursor->root() != nullptr);
  CHECK(cursor->root()->produces_rows());
  CHECK(!cursor->root()->is_write());
  CHECK_EQ(cursor->affected_rows(), size_t{0});

  auto first = cursor->next();
  CHECK(first.has_value());
  if (first.has_value()) {
    CHECK_EQ(first->size(), size_t{1});
    CHECK_EQ((*first)[0].as_str(), std::string("FullScan(t)"));
  }
  auto second = cursor->next();
  CHECK(second.has_value());
  if (second.has_value()) {
    CHECK_EQ((*second)[0].as_str(), std::string("  Filter(x)"));
  }
  auto third = cursor->next();
  CHECK(!third.has_value());
  if (!third.has_value()) {
    CHECK(third.error().end()); // 正常结束，不是错误
  }
  // 结束之后错误会粘住（和别的游标一致），close() 幂等
  CHECK(cursor->next().error().end());
  cursor->close();
  cursor->close();
}

TEST(TextResult, EmptyTextIsAnEmptyResultSet) {
  auto cursor = exec::text_result("QUERY PLAN", std::vector<std::string>{});
  CHECK(cursor != nullptr);
  if (cursor == nullptr) {
    return;
  }
  auto row = cursor->next();
  CHECK(!row.has_value());
  if (!row.has_value()) {
    CHECK(row.error().end());
  }
}
