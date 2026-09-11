#pragma once

#include "condition.h"
#include "scan_desc.h"
#include <unordered_map>
#include <functional>

namespace query
{

  struct ScanConvertResult
  {
    ScanDescriptor scan_desc; // 扫描描述
    // 对于无法处理的分支，扔到remaining里。
    std::unique_ptr<ConditionExpr> remaining; // 剩余条件树
  };

  // ============================================================
  // 主键条件树 → 扫描描述 转换器
  // ============================================================
  class ScanConverter
  {
  private:
    ConditionExtractResult extract_result_;
    ScanConvertResult result_;

  public:
    ScanConverter() = default;
    ScanConverter(ConditionExtractResult &&extract_result) : extract_result_(std::move(extract_result)) {}

    ScanConverter &convert()
    {
      auto res = convert(extract_result_.pk_cond.get());
      if (res)
      {
        result_ = std::move(res.value());
        result_.remaining = std::make_unique<ConditionExpr>(ConditionType::AND,
                                                            std::move(extract_result_.remaining), std::move(result_.remaining));
      }
      else{
        // 无法转换为扫描描述，保留原条件
        result_.remaining = std::make_unique<ConditionExpr>(ConditionType::AND,
                                                            std::move(extract_result_.remaining), std::move(extract_result_.pk_cond));
      }
      return *this;
    }

    const ScanConvertResult &result() const { return result_; }

  private:
    static std::optional<ScanConvertResult> convert(const ConditionExpr *expr)
    {
      if (!expr)
        return std::nullopt;

      ScanConvertResult result;

      if (expr->is_leaf())
      {
        auto desc = convert_leaf(expr);
        if(!desc){
          result.remaining = expr->clone();
        }else{
          result.scan_desc = std::move(desc.value());
        }
        return result;
      }

      if (expr->is_and())
      {
        auto left = convert(expr->left());
        auto right = convert(expr->right());
        auto c = ScanDescriptor::intersect(left->scan_desc, right->scan_desc);
        if(c ){
          result.scan_desc = std::move(c.value());
          result.remaining = std::make_unique<ConditionExpr>(ConditionType::AND,
                                                            std::move(left->remaining), std::move(right->remaining));
        }else{
          // 无法计算交集，保留原条件
          result.remaining = expr->clone();
        } 
        return result;
      }

      if (expr->is_or())
      {
        auto left = convert(expr->left());
        auto right = convert(expr->right());
        auto c = ScanDescriptor::unite(left->scan_desc, right->scan_desc);
        if(c){
          result.scan_desc = std::move(c.value());
          result.remaining = std::make_unique<ConditionExpr>(ConditionType::OR,
                                                            std::move(left->remaining), std::move(right->remaining));
        }else{
          // 无法计算并集，保留原条件
          result.remaining = expr->clone();
        }
        return result;
      }

      if (expr->is_not())
      {
        // NOT 在 pk_cond 中通常不会出现（已由 pushdown_not 处理）
        // 如果出现，尝试补集
        auto child = convert(expr->left());
        auto c = ScanDescriptor::complement(child->scan_desc);
        if(c){
          result.scan_desc = std::move(c.value());
          result.remaining = std::make_unique<ConditionExpr>(ConditionType::NOT, std::move(child->remaining));
        }
        else{
          // 无法计算补集，保留原条件
          result.remaining = expr->clone();
        }
        return result;
      }

      return std::nullopt;
    }

    // ============================================================
    // 叶子节点转换
    // ============================================================
    static inline std::optional<ScanDescriptor> convert_leaf(const ConditionExpr *expr)
    {
      ScanDescriptor desc;

      if (expr->is_compare())
      {
        switch (expr->op())
        {
        case CompareOp::EQ:
          desc.point_set.add(expr->value());
          break;

        case CompareOp::GE:
          desc.range_set.push_back(KeyRange::from(expr->value()));
          break;

        case CompareOp::GT:
        {
          if (expr->value().is_int())
          {
            desc.range_set.push_back(KeyRange::from(
                sql::Value(expr->value().int_val() + 1)));
          }
          else
          {
            // 非整数：保留为范围，但可能需要排除边界
            desc.range_set.push_back(KeyRange::from(expr->value()));
            desc.excluded.add(expr->value());
          }
          break;
        }

        case CompareOp::LE:
        {
          if (expr->value().is_int())
          {
            desc.range_set.push_back(KeyRange::to(
                sql::Value(expr->value().int_val() + 1)));
          }
          else
          {
            desc.range_set.push_back(KeyRange::to(expr->value()));
            desc.point_set.add(expr->value());
          }
          break;
        }

        case CompareOp::LT:
          desc.range_set.push_back(KeyRange::to(expr->value()));
          break;

        case CompareOp::NE:
        {
          // != 转为全范围排除点
          desc = ScanDescriptor::all();
          desc.excluded.add(expr->value());
          break;
        }

        default:
          // LIKE, IS_NULL, IS_NOT_NULL 无法转换为索引扫描
          return std::nullopt;
        }
        return desc;
      }

      if (expr->is_in())
      {
        // IN 转为点集
        for (const auto &v : expr->in_values())
        {
          desc.point_set.add(v);
        }
        return desc;
      }

      return std::nullopt;
    }
  };

}