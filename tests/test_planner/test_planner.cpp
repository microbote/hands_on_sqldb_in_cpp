// tests/test_planner/test_planner.cpp
//
// Planner：OptimizedQuery -> Plan 树。
//
// 关注四件事：
//   1) 扫描算子选得对不对（FullScan / IndexScan / RangeUnion）
//   2) 链的形状：Project? -> Limit? -> Sort/TopN? -> Filter? -> 扫描
//   3) Sort 消除：ORDER BY 首列是主键时扫描顺序已经够用
//   4) **写语句复用同一条扫描链**（这是这次重构的重点）
#include "test_framework.h"

#include <memory>
#include <string>

#include "planner/planner.h"
#include "planner_test_util.h"

namespace {

// 优化 + 规划；失败返回 nullptr
std::unique_ptr<plan::PlanNode> plan_sql(const sql::Catalog &catalog,
                                         const std::string &sql) {
  auto query = plantest::build_query(sql);
  if (!query.has_value()) {
    return nullptr;
  }
  plan::Optimizer optimizer(catalog);
  auto optimized = optimizer.optimize(*query);
  if (!optimized.has_value()) {
    return nullptr;
  }
  plan::Planner planner;
  auto planned = planner.plan(std::move(*optimized));
  if (!planned.has_value()) {
    return nullptr;
  }
  return std::move(*planned);
}

const plan::ScanPlan *as_scan(const plan::PlanNode *node) {
  return dynamic_cast<const plan::ScanPlan *>(node);
}

// 找到链上的扫描算子（第一个 ScanPlan）
const plan::ScanPlan *find_scan(const plan::PlanNode *root) {
  for (const plan::PlanNode *node = root; node != nullptr;
       node = node->child()) {
    if (const auto *scan = as_scan(node)) {
      return scan;
    }
  }
  return nullptr;
}

} // namespace

// ============================================================
// 扫描算子的选择
// ============================================================
TEST(Planner, SelectWithoutNarrowingIsFullScan) {
  MemoryCatalog catalog;
  plantest::setup_catalog(catalog);
  auto node = plan_sql(catalog, "SELECT * FROM users");
  CHECK(node != nullptr);
  if (node == nullptr) {
    return;
  }
  CHECK(node->type() == plan::PlanType::FULL_SCAN);
  CHECK(node->output_order() == plan::Ordering::PK_ASC);

  const auto *scan = as_scan(node.get());
  CHECK(scan != nullptr);
  if (scan != nullptr) {
    CHECK_EQ(scan->target().db.str(), std::string("testdb"));
    CHECK_EQ(scan->target().table.str(), std::string("users"));
    CHECK_EQ(scan->target().primary_key.str(), std::string("id"));
    CHECK(scan->target().primary_key_type == sql::DataType::INT);
    CHECK(scan->ascending());
  }
  CHECK(node->to_string().find("FullScan") != std::string::npos);
}

TEST(Planner, PkEqualityIsSinglePointIndexScan) {
  MemoryCatalog catalog;
  plantest::setup_catalog(catalog);
  auto node = plan_sql(catalog, "SELECT * FROM users WHERE id = 5");
  CHECK(node != nullptr);
  if (node == nullptr) {
    return;
  }
  CHECK(node->type() == plan::PlanType::INDEX_SCAN);
  const auto *scan = dynamic_cast<const plan::IndexScanPlan *>(node.get());
  CHECK(scan != nullptr);
  if (scan != nullptr) {
    CHECK(scan->is_point());
    CHECK(scan->range().to_string() == std::string("[5, 5]"));
  }
}

TEST(Planner, PkRangeIsSingleIntervalIndexScan) {
  MemoryCatalog catalog;
  plantest::setup_catalog(catalog);
  auto node = plan_sql(catalog, "SELECT * FROM users WHERE id >= 3 AND id < 7");
  CHECK(node != nullptr);
  if (node == nullptr) {
    return;
  }
  CHECK(node->type() == plan::PlanType::INDEX_SCAN);
  const auto *scan = dynamic_cast<const plan::IndexScanPlan *>(node.get());
  CHECK(scan != nullptr);
  if (scan != nullptr) {
    CHECK(scan->range().to_string() == std::string("[3, 7)"));
  }
}

TEST(Planner, SparseInListBecomesRangeUnion) {
  MemoryCatalog catalog;
  plantest::setup_catalog(catalog);
  auto node = plan_sql(catalog, "SELECT * FROM users WHERE id IN (1, 3, 5)");
  CHECK(node != nullptr);
  if (node == nullptr) {
    return;
  }
  CHECK(node->type() == plan::PlanType::RANGE_UNION);
  const auto *union_scan =
      dynamic_cast<const plan::RangeUnionPlan *>(find_scan(node.get()));
  CHECK(union_scan != nullptr);
  if (union_scan != nullptr) {
    CHECK_EQ(union_scan->ranges().size(), size_t{3});
    CHECK(union_scan->to_string().find("RangeUnion") != std::string::npos);
  }
}

TEST(Planner, RangeUnionCarriesExclusionPoints) {
  MemoryCatalog catalog;
  plantest::setup_catalog(catalog);
  // id != 5：候选是全表 + 一个要跳过的点 -> 用 RangeUnion 承载
  auto node = plan_sql(catalog, "SELECT * FROM users WHERE id != 5");
  CHECK(node != nullptr);
  if (node == nullptr) {
    return;
  }
  const auto *union_scan =
      dynamic_cast<const plan::RangeUnionPlan *>(find_scan(node.get()));
  CHECK(union_scan != nullptr);
  if (union_scan != nullptr) {
    CHECK_EQ(union_scan->exclude_keys().size(), size_t{1});
    CHECK(union_scan->exclude_keys().contains(sql::Value(int64_t{5})));
  }
}

TEST(Planner, EmptyScanStillPlansAScan) {
  MemoryCatalog catalog;
  plantest::setup_catalog(catalog);
  // 条件恒假：计划要存在，只是扫不到行
  auto node = plan_sql(catalog, "SELECT * FROM users WHERE id > 5 AND id < 3");
  CHECK(node != nullptr);
  if (node == nullptr) {
    return;
  }
  CHECK(node->type() == plan::PlanType::INDEX_SCAN);
  const auto *scan = dynamic_cast<const plan::IndexScanPlan *>(node.get());
  CHECK(scan != nullptr);
  if (scan != nullptr) {
    CHECK(scan->range().is_empty());
  }
}

// ============================================================
// 链的形状
// ============================================================
TEST(Planner, NonPrimaryKeyFilterAddsFilterNode) {
  MemoryCatalog catalog;
  plantest::setup_catalog(catalog);
  auto node = plan_sql(catalog, "SELECT * FROM users WHERE name = 'x'");
  CHECK(node != nullptr);
  if (node == nullptr) {
    return;
  }
  CHECK(node->type() == plan::PlanType::FILTER);
  CHECK(node->child() != nullptr);
  if (node->child() != nullptr) {
    CHECK(node->child()->type() == plan::PlanType::FULL_SCAN);
  }
  CHECK(node->output_order() == plan::Ordering::PK_ASC); // 过滤不改顺序
  const auto *filter = dynamic_cast<const plan::FilterPlan *>(node.get());
  CHECK(filter != nullptr);
  if (filter != nullptr) {
    CHECK(filter->condition() != nullptr);
  }
}

TEST(Planner, FullyPushedDownPkConditionHasNoFilterNode) {
  MemoryCatalog catalog;
  plantest::setup_catalog(catalog);
  auto node = plan_sql(catalog, "SELECT * FROM users WHERE id = 5");
  CHECK(node != nullptr);
  if (node != nullptr) {
    CHECK(plantest::find_node(node.get(), plan::PlanType::FILTER) == nullptr);
  }
}

TEST(Planner, FullTreeShapeForFilterSortLimitProject) {
  MemoryCatalog catalog;
  plantest::setup_catalog(catalog);
  auto node = plan_sql(
      catalog,
      "SELECT id FROM users WHERE name = 'x' ORDER BY age LIMIT 2 OFFSET 1");
  CHECK(node != nullptr);
  if (node == nullptr) {
    return;
  }

  // Projection -> Limit -> TopN -> Filter -> FullScan
  CHECK(node->type() == plan::PlanType::PROJECT);
  const plan::PlanNode *limit = node->child();
  CHECK(limit != nullptr && limit->type() == plan::PlanType::LIMIT);
  const plan::PlanNode *sort = limit != nullptr ? limit->child() : nullptr;
  CHECK(sort != nullptr && sort->type() == plan::PlanType::SORT);
  const plan::PlanNode *filter = sort != nullptr ? sort->child() : nullptr;
  CHECK(filter != nullptr && filter->type() == plan::PlanType::FILTER);
  const plan::PlanNode *scan = filter != nullptr ? filter->child() : nullptr;
  CHECK(scan != nullptr && scan->type() == plan::PlanType::FULL_SCAN);

  const std::string text = plan::plan_tree_to_string(*node);
  CHECK(text.find("Project") != std::string::npos);
  CHECK(text.find("Limit(limit=2 offset=1)") != std::string::npos);
  CHECK(text.find("TopN(order_by=[age ASC] n=3)") != std::string::npos);
}

TEST(Planner, LimitWithoutOrderByOnlyAddsLimitNode) {
  MemoryCatalog catalog;
  plantest::setup_catalog(catalog);
  auto node = plan_sql(catalog, "SELECT * FROM users LIMIT 3");
  CHECK(node != nullptr);
  if (node == nullptr) {
    return;
  }
  CHECK(node->type() == plan::PlanType::LIMIT);
  CHECK(plantest::find_node(node.get(), plan::PlanType::SORT) == nullptr);
}

TEST(Planner, ProjectionNodeOnlyWhenColumnsAreListed) {
  MemoryCatalog catalog;
  plantest::setup_catalog(catalog);

  auto star = plan_sql(catalog, "SELECT * FROM users");
  CHECK(star != nullptr);
  if (star != nullptr) {
    CHECK(plantest::find_node(star.get(), plan::PlanType::PROJECT) == nullptr);
  }

  auto listed = plan_sql(catalog, "SELECT id, name FROM users");
  CHECK(listed != nullptr);
  if (listed != nullptr) {
    CHECK(listed->type() == plan::PlanType::PROJECT);
    const auto *project = dynamic_cast<const plan::ProjectPlan *>(listed.get());
    CHECK(project != nullptr);
    if (project != nullptr) {
      CHECK_EQ(project->columns().size(), size_t{2});
    }
  }
}

// ============================================================
// ORDER BY 主键：不排序，靠扫描方向 + LIMIT 早停
// ============================================================
TEST(Planner, OrderByPrimaryKeyAscendingNeedsNoSort) {
  MemoryCatalog catalog;
  plantest::setup_catalog(catalog);
  auto node =
      plan_sql(catalog, "SELECT * FROM users WHERE id >= 3 ORDER BY id");
  CHECK(node != nullptr);
  if (node == nullptr) {
    return;
  }
  CHECK(node->type() == plan::PlanType::INDEX_SCAN);
  CHECK(plantest::find_node(node.get(), plan::PlanType::SORT) == nullptr);
  CHECK(node->output_order() == plan::Ordering::PK_ASC);
}

TEST(Planner, OrderByPrimaryKeyDescendingScansBackwards) {
  MemoryCatalog catalog;
  plantest::setup_catalog(catalog);
  auto node =
      plan_sql(catalog, "SELECT * FROM users WHERE id >= 3 ORDER BY id DESC");
  CHECK(node != nullptr);
  if (node == nullptr) {
    return;
  }
  CHECK(node->type() == plan::PlanType::INDEX_SCAN);
  CHECK(plantest::find_node(node.get(), plan::PlanType::SORT) == nullptr);
  CHECK(node->output_order() == plan::Ordering::PK_DESC);
  const auto *scan = as_scan(node.get());
  CHECK(scan != nullptr);
  if (scan != nullptr) {
    CHECK(!scan->ascending());
  }
}

TEST(Planner, OrderByPrimaryKeyPlusMoreColumnsStillNeedsNoSort) {
  MemoryCatalog catalog;
  plantest::setup_catalog(catalog);
  auto node = plan_sql(catalog, "SELECT * FROM users ORDER BY id, age DESC");
  CHECK(node != nullptr);
  if (node != nullptr) {
    CHECK(plantest::find_node(node.get(), plan::PlanType::SORT) == nullptr);
    CHECK(node->type() == plan::PlanType::FULL_SCAN);
  }
}

// ============================================================
// ORDER BY 非主键：必须排序，有 LIMIT 时退化成 TopN
// ============================================================
TEST(Planner, OrderByNonPrimaryKeyNeedsSort) {
  MemoryCatalog catalog;
  plantest::setup_catalog(catalog);
  auto node = plan_sql(catalog, "SELECT * FROM users ORDER BY age");
  CHECK(node != nullptr);
  if (node == nullptr) {
    return;
  }
  CHECK(node->type() == plan::PlanType::SORT);
  const auto *sort = dynamic_cast<const plan::SortPlan *>(node.get());
  CHECK(sort != nullptr);
  if (sort != nullptr) {
    CHECK(!sort->is_top_n());
    CHECK_EQ(sort->order_by().size(), size_t{1});
  }
  CHECK(node->output_order() == plan::Ordering::NONE);
}

TEST(Planner, OrderByNonPrimaryKeyWithLimitIsTopN) {
  MemoryCatalog catalog;
  plantest::setup_catalog(catalog);
  auto node = plan_sql(catalog, "SELECT * FROM users ORDER BY age LIMIT 10");
  CHECK(node != nullptr);
  if (node == nullptr) {
    return;
  }
  CHECK(node->type() == plan::PlanType::LIMIT);
  const auto *sort = dynamic_cast<const plan::SortPlan *>(node->child());
  CHECK(sort != nullptr);
  if (sort != nullptr) {
    CHECK(sort->is_top_n());
    CHECK_EQ(sort->top_n(), size_t{10});
  }
}

TEST(Planner, TopNCountIncludesOffset) {
  MemoryCatalog catalog;
  plantest::setup_catalog(catalog);
  auto node =
      plan_sql(catalog, "SELECT * FROM users ORDER BY age LIMIT 10 OFFSET 5");
  CHECK(node != nullptr);
  if (node == nullptr) {
    return;
  }
  const auto *sort = dynamic_cast<const plan::SortPlan *>(node->child());
  CHECK(sort != nullptr);
  if (sort != nullptr) {
    CHECK_EQ(sort->top_n(), size_t{15}); // OFFSET 5 + LIMIT 10
  }
}

TEST(Planner, SortSitsOnTopOfNarrowedScan) {
  MemoryCatalog catalog;
  plantest::setup_catalog(catalog);
  auto node =
      plan_sql(catalog, "SELECT * FROM users WHERE id >= 3 ORDER BY age");
  CHECK(node != nullptr);
  if (node == nullptr) {
    return;
  }
  CHECK(node->type() == plan::PlanType::SORT);
  CHECK(node->child() != nullptr);
  if (node->child() != nullptr) {
    CHECK(node->child()->type() == plan::PlanType::INDEX_SCAN);
  }
}

// ============================================================
// 写语句：复用同一条扫描链
// ============================================================
TEST(Planner, UpdatePullsFromSharedScanChain) {
  MemoryCatalog catalog;
  plantest::setup_catalog(catalog);
  auto node =
      plan_sql(catalog, "UPDATE users SET age = 1 WHERE id = 3 AND name = 'x'");
  CHECK(node != nullptr);
  if (node == nullptr) {
    return;
  }
  CHECK(node->type() == plan::PlanType::UPDATE);

  // Update -> Filter -> IndexScan(点)
  const plan::PlanNode *filter = node->child();
  CHECK(filter != nullptr && filter->type() == plan::PlanType::FILTER);
  const plan::PlanNode *scan = filter != nullptr ? filter->child() : nullptr;
  CHECK(scan != nullptr && scan->type() == plan::PlanType::INDEX_SCAN);
  const auto *index = dynamic_cast<const plan::IndexScanPlan *>(scan);
  CHECK(index != nullptr);
  if (index != nullptr) {
    CHECK(index->is_point());
  }

  // 语句仍在写节点上（assignments 从这里读）
  const auto *update = dynamic_cast<const plan::UpdatePlan *>(node.get());
  CHECK(update != nullptr);
  if (update != nullptr) {
    CHECK(update->query().query.is_update());
  }
}

TEST(Planner, UpdateWithoutWhereScansWholeTable) {
  MemoryCatalog catalog;
  plantest::setup_catalog(catalog);
  auto node = plan_sql(catalog, "UPDATE users SET age = 1");
  CHECK(node != nullptr);
  if (node == nullptr) {
    return;
  }
  CHECK(node->type() == plan::PlanType::UPDATE);
  // 没有 WHERE = 全表扫描，绝不是"扫零行"
  CHECK(node->child() != nullptr);
  if (node->child() != nullptr) {
    CHECK(node->child()->type() == plan::PlanType::FULL_SCAN);
  }
}

TEST(Planner, DeletePullsFromSharedScanChain) {
  MemoryCatalog catalog;
  plantest::setup_catalog(catalog);
  auto node = plan_sql(catalog, "DELETE FROM users WHERE id IN (1, 3, 5)");
  CHECK(node != nullptr);
  if (node == nullptr) {
    return;
  }
  CHECK(node->type() == plan::PlanType::DELETE);
  const plan::PlanNode *scan = node->child();
  CHECK(scan != nullptr && scan->type() == plan::PlanType::RANGE_UNION);
}

TEST(Planner, SelectAndDeleteShareTheSameScanChainShape) {
  MemoryCatalog catalog;
  plantest::setup_catalog(catalog);
  auto select =
      plan_sql(catalog, "SELECT * FROM users WHERE id >= 3 AND name = 'x'");
  auto del =
      plan_sql(catalog, "DELETE FROM users WHERE id >= 3 AND name = 'x'");
  CHECK(select != nullptr && del != nullptr);
  if (select == nullptr || del == nullptr) {
    return;
  }

  // SELECT 的链 = Filter -> IndexScan；DELETE 的链 = Delete -> 同一条链
  const plan::PlanNode *select_scan = find_scan(select.get());
  const plan::PlanNode *delete_scan = find_scan(del.get());
  CHECK(select_scan != nullptr && delete_scan != nullptr);
  if (select_scan != nullptr && delete_scan != nullptr) {
    CHECK(select_scan->type() == delete_scan->type());
    CHECK_EQ(select_scan->to_string(), delete_scan->to_string());
  }
  // 主键条件下推进了扫描空间，剩下的 name 谓词留在 Filter 上
  CHECK_EQ(select->to_string(), std::string("Filter(name = x)"));
  CHECK(del->to_string().find("Delete(") == 0);
}

TEST(Planner, InsertHasNoChildForValuesForm) {
  MemoryCatalog catalog;
  plantest::setup_catalog(catalog);
  auto node = plan_sql(catalog,
                       "INSERT INTO users (id, name, age) VALUES (1, 'a', 20)");
  CHECK(node != nullptr);
  if (node == nullptr) {
    return;
  }
  CHECK(node->type() == plan::PlanType::INSERT);
  CHECK(node->child() == nullptr); // 行源在语句的 VALUES 里
  const auto *insert = dynamic_cast<const plan::InsertPlan *>(node.get());
  CHECK(insert != nullptr);
  if (insert != nullptr) {
    CHECK(insert->query().query.is_insert());
  }
}

TEST(Planner, DdlPlans) {
  MemoryCatalog catalog;
  plantest::setup_catalog(catalog);

  auto create_db = plan_sql(catalog, "CREATE DATABASE otherdb");
  CHECK(create_db != nullptr);
  if (create_db != nullptr) {
    CHECK(create_db->type() == plan::PlanType::CREATE_DATABASE);
  }

  auto drop_db = plan_sql(catalog, "DROP DATABASE otherdb");
  CHECK(drop_db != nullptr);
  if (drop_db != nullptr) {
    CHECK(drop_db->type() == plan::PlanType::DROP_DATABASE);
  }

  auto create_table =
      plan_sql(catalog, "CREATE TABLE t (id INT PRIMARY KEY, name VARCHAR(8))");
  CHECK(create_table != nullptr);
  if (create_table != nullptr) {
    CHECK(create_table->type() == plan::PlanType::CREATE_TABLE);
    const auto *created =
        dynamic_cast<const plan::CreateTablePlan *>(create_table.get());
    CHECK(created != nullptr);
    if (created != nullptr) {
      CHECK(created->query().table.str() == "t");
    }
  }

  auto drop_table = plan_sql(catalog, "DROP TABLE users");
  CHECK(drop_table != nullptr);
  if (drop_table != nullptr) {
    CHECK(drop_table->type() == plan::PlanType::DROP_TABLE);
  }

  auto use_db = plan_sql(catalog, "USE testdb");
  CHECK(use_db != nullptr);
  if (use_db != nullptr) {
    CHECK(use_db->type() == plan::PlanType::USE_DATABASE);
    const auto *use = dynamic_cast<const plan::UseDatabasePlan *>(use_db.get());
    CHECK(use != nullptr);
    if (use != nullptr) {
      CHECK(use->query().db.str() == "testdb");
    }
  }
}
