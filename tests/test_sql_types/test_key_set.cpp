// test_key_set.cpp
#include "test_framework.h"
#include "sql_types/key_set.h"
#include "sql_types/value.h"

using namespace sql;

TEST(KeySet, InitiallyEmpty) {
    KeySet keys;
    CHECK(keys.is_empty());
    CHECK_EQ(keys.size(), 0);
}

TEST(KeySet, AddAndContains) {
    KeySet keys;
    
    keys.add(Value(1));
    keys.add(Value(2));
    keys.add(Value(3));
    
    CHECK(!keys.is_empty());
    CHECK_EQ(keys.size(), 3);
    CHECK(keys.contains(Value(1)));
    CHECK(keys.contains(Value(2)));
    CHECK(keys.contains(Value(3)));
    CHECK(!keys.contains(Value(4)));
}

TEST(KeySet, AddDuplicate) {
    KeySet keys;
    
    keys.add(Value(1));
    keys.add(Value(1));  // 重复
    
    CHECK_EQ(keys.size(), 1);
}

TEST(KeySet, Remove) {
    KeySet keys;
    keys.add(Value(1));
    keys.add(Value(2));
    
    keys.remove(Value(1));
    CHECK_EQ(keys.size(), 1);
    CHECK(!keys.contains(Value(1)));
    CHECK(keys.contains(Value(2)));
    
    // 移除不存在的
    keys.remove(Value(10));
    CHECK_EQ(keys.size(), 1);
}

TEST(KeySet, Clear) {
    KeySet keys;
    keys.add(Value(1));
    keys.add(Value(2));
    
    keys.clear();
    CHECK(keys.is_empty());
    CHECK_EQ(keys.size(), 0);
}

TEST(KeySet, MinAndMax) {
    KeySet keys;
    keys.add(Value(5));
    keys.add(Value(2));
    keys.add(Value(8));
    keys.add(Value(3));
    
    CHECK_EQ(keys.min().as_int(), 2);
    CHECK_EQ(keys.max().as_int(), 8);
}

TEST(KeySet, TypeMismatch) {
    KeySet int_keys;
    
    // 添加错误类型应该抛出或忽略
    // CHECK_THROW(int_keys.add(Value(std::string("abc"))));
}

TEST(KeySet, Iteration) {
    KeySet keys;
    keys.add(Value(3));
    keys.add(Value(1));
    keys.add(Value(2));
    
    std::vector<int> values;
    for (const auto& v : keys) {
        values.push_back(v.as_int());
    }
    
    // 排序后遍历
    CHECK_EQ(values.size(), 3);
    // 如果有固定的迭代顺序，可以检查具体值
}

TEST(KeySet, ToString) {
    KeySet keys;
    keys.add(Value(1));
    keys.add(Value(2));
    
    std::string s = keys.to_string();
    CHECK(!s.empty());
    CHECK(s.find("1") != std::string::npos);
    CHECK(s.find("2") != std::string::npos);
}