// test_condition.cpp
#include "test_framework.h"
#include "sql_types/condition.h"
#include "sql_types/condition_types.h"
#include "sql_types/condition_visitor.h"

using namespace sql;

TEST(Condition, CompareCondition) {
    auto cond = make_compare("age", CompareOp::GT, Value(18));
    
    CHECK(cond->is_compare());
    CHECK(cond->is_leaf());
    CHECK(!cond->is_and());
    CHECK(!cond->is_or());
    CHECK(!cond->is_not());
    
    const auto* cmp = dynamic_cast<const CompareCondition*>(cond.get());
    CHECK(cmp != nullptr);
    CHECK_EQ(cmp->column(), "age");
    CHECK(cmp->op() == CompareOp::GT);
    CHECK_EQ(cmp->value().as_int(), 18);
    
    CHECK_EQ(cmp->to_string(), "age > 18");
}

TEST(Condition, InCondition) {
    std::vector<Value> values = {Value(1), Value(2), Value(3)};
    auto cond = make_in("id", false, values);
    
    CHECK(cond->is_in());
    CHECK(cond->is_leaf());
    CHECK(!cond->is_not());
    
    const auto* in = dynamic_cast<const InCondition*>(cond.get());
    CHECK(in != nullptr);
    CHECK_EQ(in->column(), "id");
    CHECK(!in->is_not_in());
    CHECK_EQ(in->value_count(), 3);
    
    // contains 检查
    CHECK(in->contains(Value(2)));
    CHECK(!in->contains(Value(10)));
    
    // NOT IN
    auto not_in = make_in("id", true, values);
    const auto* not_in_ptr = dynamic_cast<const InCondition*>(not_in.get());
    CHECK(not_in_ptr->is_not_in());
    
    // to_string
    CHECK_EQ(in->to_string(), "id IN (1, 2, 3)");
}

TEST(Condition, AndCondition) {
    auto left = make_compare("age", CompareOp::GT, Value(18));
    auto right = make_compare("age", CompareOp::LT, Value(65));
    auto cond = make_and(std::move(left), std::move(right));
    
    CHECK(cond->is_and());
    CHECK(cond->is_internal());
    CHECK(!cond->is_leaf());
    
    const auto* and_cond = dynamic_cast<const AndCondition*>(cond.get());
    CHECK(and_cond != nullptr);
    CHECK_EQ(and_cond->child_count(), 2);
    CHECK(and_cond->left() != nullptr);
    CHECK(and_cond->right() != nullptr);
    
    CHECK_EQ(and_cond->left()->type(), ConditionType::COMPARE);
    CHECK_EQ(and_cond->right()->type(), ConditionType::COMPARE);
}

TEST(Condition, OrCondition) {
    auto left = make_compare("status", CompareOp::EQ, Value(std::string("active")));
    auto right = make_compare("status", CompareOp::EQ, Value(std::string("pending")));
    auto cond = make_or(std::move(left), std::move(right));
    
    CHECK(cond->is_or());
    CHECK(cond->is_internal());
    
    const auto* or_cond = dynamic_cast<const OrCondition*>(cond.get());
    CHECK(or_cond != nullptr);
    CHECK_EQ(or_cond->child_count(), 2);
}

TEST(Condition, NotCondition) {
    auto child = make_compare("status", CompareOp::EQ, Value(std::string("deleted")));
    auto cond = make_not(std::move(child));
    
    CHECK(cond->is_not());
    CHECK(cond->is_internal());
    
    const auto* not_cond = dynamic_cast<const NotCondition*>(cond.get());
    CHECK(not_cond != nullptr);
    CHECK_EQ(not_cond->child_count(), 1);
    CHECK(not_cond->child() != nullptr);
    CHECK_EQ(not_cond->child()->type(), ConditionType::COMPARE);
}

TEST(Condition, CloneAndDeepCopy) {
    auto original = make_and(
        make_compare("a", CompareOp::GT, Value(1)),
        make_or(
            make_compare("b", CompareOp::EQ, Value(2)),
            make_compare("c", CompareOp::LT, Value(3))
        )
    );
    
    auto clone = original->clone();
    CHECK(clone->is_and());
    CHECK_EQ(clone->to_string(), original->to_string());
    
    // 修改原节点不影响克隆
    // (通过动态转换修改其中一个子节点)
}

TEST(Condition, ToString) {
    // 比较
    CHECK_EQ(make_compare("age", CompareOp::GT, Value(18))->to_string(), "age > 18");
    
    // IN
    CHECK_EQ(make_in("id", false, {Value(1), Value(2)})->to_string(), "id IN (1, 2)");
    
    // AND
    auto and_cond = make_and(
        make_compare("a", CompareOp::GT, Value(1)),
        make_compare("b", CompareOp::LT, Value(10))
    );
    CHECK_EQ(and_cond->to_string(), "(a > 1 AND b < 10)");
    
    // NOT
    auto not_cond = make_not(make_compare("x", CompareOp::EQ, Value(5)));
    CHECK_EQ(not_cond->to_string(), "NOT (x = 5)");
}

TEST(Condition, VisitorPattern) {
    // 测试 ConditionVisitor
    struct CheckVisitor : public ConditionVisitor {
        int compare_count = 0;
        int in_count = 0;
        int and_count = 0;
        int or_count = 0;
        int not_count = 0;
        
        void visit(const CompareCondition&) override { compare_count++; }
        void visit(const InCondition&) override { in_count++; }
        void visit(const AndCondition& ) override {
            and_count++;
        }
        void visit(const OrCondition& ) override {
            or_count++;
        }
        void visit(const NotCondition& ) override {
            not_count++;
        }
    };
    
    auto cond = make_and(
        make_compare("a", CompareOp::GT, Value(1)),
        make_or(
            make_compare("b", CompareOp::EQ, Value(2)),
            make_not(make_in("c", false, {Value(3), Value(4)}))
        )
    );
    
    CheckVisitor visitor;
    walk_condition_pre_order(*cond, visitor);
    
    CHECK_EQ(visitor.compare_count, 2);
    CHECK_EQ(visitor.in_count, 1);
    CHECK_EQ(visitor.and_count, 1);
    CHECK_EQ(visitor.or_count, 1);
    CHECK_EQ(visitor.not_count, 1);
}