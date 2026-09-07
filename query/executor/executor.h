// executor.h
#pragma once

#include "query/planner/plan.h"
#include "relation/sql_relation.h"

namespace query
{
  enum class Status
  {
    OK = 0,
    IOError,
    InvalidArgument,
    NotFound,
    UnknownError
  };

  // ============================================================
  // 执行器接口（Volcano 模型）
  // ============================================================
  class Executor
  {
  public:
    virtual ~Executor() = default;

    // 初始化执行
    virtual Status open() = 0;

    // 获取下一行
    virtual std::optional<sql::Row> next() = 0;

    // 关闭执行器
    virtual Status close() = 0;

    // 获取执行计划
    virtual PlanNode *plan() const = 0;
  };

  // ============================================================
  // IndexScanExecutor
  // ============================================================
  class IndexScanExecutor : public Executor
  {
  public:
    IndexScanExecutor(std::shared_ptr<sql::Table> table,
                      const IndexScanPlan *plan)
        : table_(table), plan_(plan), iter_(nullptr) {}

    Status open() override
    {
      // 获取表
      if (!table_)
      {
        return Status::IOError;
      }

      // 构建扫描范围
      if (plan_->is_point_query)
      {
        // 点查询：逐个查询
        current_point_index_ = 0;
        current_point_ = next_point_query();
        return Status::OK;
      }
      else
      {
        // 范围查询：使用 Cursor
        auto range = plan_->key_range;
        iter_ = table_->scan(range);
        if (!iter_)
        {
          return Status::IOError;
        }
        return Status::OK;
      }
    }

    std::optional<sql::Row> next() override
    {
      if (plan_->is_point_query)
      {
        return next_point_query();
      }
      else
      {
        return next_range_query();
      }
    }

    Status close() override
    {
      iter_.reset();
      return Status::OK;
    }

    PlanNode *plan() const override
    {
      return const_cast<IndexScanPlan *>(plan_);
    }

    // 获取当前点查询的 key
    std::optional<sql::Row> next_point_query()
    {
      if (current_point_index_ >= plan_->key_set.size())
      {
        return std::nullopt;
      }

      // 获取当前 key 对应的行
      const std::string &key = plan_->key_set[current_point_index_];
      current_point_index_++;

      auto row = table_->get(key);
      if (!row)
      {
        return next_point_query(); // 跳过不存在的 key
      }

      // 检查是否在排除集中
      if (is_excluded(key))
      {
        return next_point_query();
      }

      // 投影列
      return project_row(row.value());
    }

    std::optional<sql::Row> next_range_query()
    {
      if (!iter_ || !iter_->valid())
      {
        return std::nullopt;
      }

      auto row = iter_->next();
      if (!row)
      {
        return std::nullopt;
      }

      // 检查是否在排除集中
      if (is_excluded(*row))
      {
        return next_range_query();
      }

      // 投影列
      return project_row(row.value());
    }

  private:
    bool is_excluded(const sql::Row &row)
    {
      // 提取主键值
      auto pk = get_primary_key(row);
      return is_excluded(pk);
    }

    bool is_excluded(const std::string &key)
    {
      return std::find(plan_->excluded_keys.begin(),
                       plan_->excluded_keys.end(),
                       key) != plan_->excluded_keys.end();
    }

    std::optional<sql::Row> project_row(const sql::Row &row)
    {
      if (plan_->columns.empty() ||
          (plan_->columns.size() == 1 && plan_->columns[0] == "*"))
      {
        return row;
      }
      // 投影指定的列
      sql::Row result;
      auto schema = table_->schema();
      for (const auto &col : plan_->columns)
      {
        int idx = schema.column_index(col);
        if (idx >= 0)
        {
          result.push_back(row[idx]);
        }
      }
      return result;
    }

    std::string get_primary_key(const sql::Row &row)
    {
      auto schema = table_->schema();
      int pk_idx = schema.primary_key_index();
      if (pk_idx >= 0 && pk_idx < static_cast<int>(row.size()))
      {
        return row[pk_idx].to_string();
      }
      return "";
    }

    std::shared_ptr<sql::Table> table_;
    const IndexScanPlan *plan_;
    std::unique_ptr<sql::Cursor> iter_;
    size_t current_point_index_ = 0;
    std::string current_point_;
  };

  // ============================================================
  // FilterExecutor
  // ============================================================
  class FilterExecutor : public Executor
  {
  public:
    FilterExecutor(std::unique_ptr<Executor> child,
                   const FilterPlan *plan,
                   const sql::TableSchema &schema)
        : child_(std::move(child)), plan_(plan), schema_(schema) {}

    Status open() override
    {
      return child_->open();
    }

    std::optional<sql::Row> next() override
    {
      while (true)
      {
        auto row = child_->next();
        if (!row)
        {
          return std::nullopt;
        }

        // 应用过滤条件
        if (plan_->condition)
        {
          if (plan_->condition->match_row(*row, schema_))
          {
            return row;
          }
        }
        else
        {
          return row;
        }
      }
    }

    Status close() override
    {
      return child_->close();
    }

    PlanNode *plan() const override
    {
      return const_cast<FilterPlan *>(plan_);
    }

  private:
    std::unique_ptr<Executor> child_;
    const FilterPlan *plan_;
    const sql::TableSchema &schema_;
  };

  // ============================================================
  // ProjectExecutor
  // ============================================================
  class ProjectExecutor : public Executor
  {
  public:
    ProjectExecutor(std::unique_ptr<Executor> child,
                    const ProjectPlan *plan)
        : child_(std::move(child)), plan_(plan) {}

    Status open() override
    {
      return child_->open();
    }

    std::optional<sql::Row> next() override
    {
      auto row = child_->next();
      if (!row)
      {
        return std::nullopt;
      }

      // 投影列
      if (plan_->columns.empty() ||
          (plan_->columns.size() == 1 && plan_->columns[0] == "*"))
      {
        return row;
      }

      sql::Row result;
      // 需要 schema 信息来获取列索引
      // 这里简化处理，实际需要传入 schema
      return row;
    }

    Status close() override
    {
      return child_->close();
    }

    PlanNode *plan() const override
    {
      return const_cast<ProjectPlan *>(plan_);
    }

  private:
    std::unique_ptr<Executor> child_;
    const ProjectPlan *plan_;
  };

  // ============================================================
  // 执行器工厂
  // ============================================================
  class ExecutorFactory
  {
  public:
    static std::unique_ptr<Executor> create_executor(
        const PlanNode *plan,
        std::shared_ptr<sql::Table> table,
        const sql::TableSchema &schema)
    {

      if (plan->type() == PlanType::INDEX_SCAN)
      {
        auto index_plan = static_cast<const IndexScanPlan *>(plan);
        return std::make_unique<IndexScanExecutor>(table, index_plan);
      }

      if (plan->type() == PlanType::FILTER)
      {
        auto filter_plan = static_cast<const FilterPlan *>(plan);
        auto child = create_executor(filter_plan->child.get(), table, schema);
        return std::make_unique<FilterExecutor>(std::move(child), filter_plan, schema);
      }

      if (plan->type() == PlanType::PROJECT)
      {
        auto project_plan = static_cast<const ProjectPlan *>(plan);
        auto child = create_executor(project_plan->child.get(), table, schema);
        return std::make_unique<ProjectExecutor>(std::move(child), project_plan);
      }

      return nullptr;
    }
  };

} // namespace query