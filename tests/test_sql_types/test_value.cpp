// tests/test_sql_types/test_value.cpp
#include "test_framework.h"
#include "sql_types/value.h"

using namespace sql;

TEST(Value, NullConstruction) {
    Value v;
    CHECK(v.is_null());
    CHECK_EQ(v.type(), DataType::NULL_TYPE);
    CHECK_EQ(v.to_string(), "NULL");
}

TEST(Value, IntConstruction) {
    Value v(42);
    CHECK(v.is_int());
    CHECK(!v.is_null());
    CHECK_EQ(v.as_int(), 42);
    CHECK_EQ(v.to_string(), "42");
    
    Value v2(static_cast<int64_t>(100));
    CHECK_EQ(v2.as_int(), 100);
}

TEST(Value, BoolConstruction) {
    Value v(true);
    CHECK(v.is_bool());
    CHECK_EQ(v.as_bool(), true);
    CHECK_EQ(v.to_string(), "true");
    
    Value v2(false);
    CHECK_EQ(v2.as_bool(), false);
    CHECK_EQ(v2.to_string(), "false");
}

TEST(Value, StringConstruction) {
    Value v(std::string("hello"));
    CHECK(v.is_string());
    CHECK_EQ(v.as_str(), "hello");
    
    Value v2("world");
    CHECK_EQ(v2.as_str(), "world");
    
    // 中文等 UTF-8 字符串
    Value v3("你好世界");
    CHECK_EQ(v3.as_str(), "你好世界");
}

TEST(Value, CopyConstruction) {
    // 整型拷贝
    Value orig_int(42);
    Value copy_int(orig_int);
    CHECK_EQ(copy_int.as_int(), 42);
    CHECK(!copy_int.is_null());
    
    // 字符串拷贝（深拷贝）
    Value orig_str("hello");
    Value copy_str(orig_str);
    CHECK_EQ(copy_str.as_str(), "hello");
    
    // 修改源不影响拷贝
    orig_str = Value("world");
    CHECK_EQ(copy_str.as_str(), "hello");
}

TEST(Value, MoveConstruction) {
    // 整型移动
    Value orig_int(42);
    Value moved_int(std::move(orig_int));
    CHECK_EQ(moved_int.as_int(), 42);
    
    // 字符串移动
    Value orig_str("test");
    Value moved_str(std::move(orig_str));
    CHECK_EQ(moved_str.as_str(), "test");
    CHECK(orig_str.is_null());  // 源变为 NULL
}

TEST(Value, Assignment) {
    Value v;
    v = Value(42);
    CHECK_EQ(v.as_int(), 42);
    
    v = Value("hello");
    CHECK(v.is_string());
    CHECK_EQ(v.as_str(), "hello");
    
    v = Value(true);
    CHECK(v.is_bool());
    CHECK_EQ(v.as_bool(), true);
    
    // 字符串赋值给字符串（优化路径）
    Value a("foo");
    Value b("bar");
    a = b;
    CHECK_EQ(a.as_str(), "bar");
}

TEST(Value, Equality) {
    Value int1(42);
    Value int2(42);
    Value int3(43);
    
    CHECK(int1 == int2);
    CHECK(int1 != int3);
    
    Value str1("hello");
    Value str2("hello");
    Value str3("world");
    
    CHECK(str1 == str2);
    CHECK(str1 != str3);
    
    // NULL
    Value n1;
    Value n2;
    CHECK(n1 == n2);
    CHECK(!(n1 == int1));  // NULL != 42
    
    // 类型不同
    CHECK(!(int1 == str1));
}

TEST(Value, Comparison) {
    Value int1(1);
    Value int2(2);
    CHECK(int1 < int2);
    CHECK(int1 <= int2);
    CHECK(int2 > int1);
    CHECK(int2 >= int1);
    
    // 字符串比较
    Value str1("apple");
    Value str2("banana");
    CHECK(str1 < str2);
    
    // NULL 比较
    Value null1;
    CHECK(null1 < int1);  // NULL 总是最小
}

TEST(Value, StringConversion) {
    Value int_val(123);
    CHECK_EQ(int_val.to_string(), "123");
    
    Value bool_true(true);
    CHECK_EQ(bool_true.to_string(), "true");
    
    Value str_val("hello");
    CHECK_EQ(str_val.to_string(), "hello");
    
    Value null_val;
    CHECK_EQ(null_val.to_string(), "NULL");
}

TEST(Value, FromString) {
    // 整数
    Value int44 = Value::from_string("44", DataType::INT);
    CHECK_EQ(int44.as_int(), 44);
    
    // 字符串
    Value str = Value::from_string("hello", DataType::VARCHAR);
    CHECK_EQ(str.as_str(), "hello");
    
    // 布尔
    Value bool_true = Value::from_string("true", DataType::BOOLEAN);
    CHECK(bool_true.as_bool());
    
    Value bool_false = Value::from_string("false", DataType::BOOLEAN);
    CHECK(!bool_false.as_bool());
    
    // NULL 字符串
    Value null_val = Value::from_string("NULL", DataType::INT);
    CHECK(null_val.is_null());
}

TEST(Value, HashValue) {
    Value int1(42);
    Value int2(42);
    Value str1("hello");
    
    CHECK_EQ(int1.hash(), int2.hash());  // 相同值哈希相同
    CHECK(int1.hash() != str1.hash());   // 不同类型哈希不同
}

TEST(Value, SafeAccessors) {
    Value int_val(42);
    Value str_val("hello");
    Value bool_val(true);
    
    // 正确的类型
    CHECK_EQ(int_val.get_int(), 42);
    CHECK_EQ(str_val.as_str(), "hello");
    CHECK_EQ(bool_val.get_bool(), true);
    
    // 错误的类型显示默认值
    CHECK_EQ(str_val.get_int(-1), -1);  // 字符串不是整数
    CHECK_EQ(int_val.get_bool(true), true);  // 整数不是布尔
    
    // 安全的字符串视图
    auto view = str_val.get_str_view();
    CHECK_EQ(std::string(view), "hello");
    
    // 字符串指针（可选）
    const std::string* ptr = str_val.get_str_ptr();
    CHECK(ptr != nullptr);
    if (ptr) {
        CHECK_EQ(*ptr, "hello");
    }
}

TEST(Value, TypeIsTemplate) {
    Value int_val(42);
    CHECK(int_val.is<int>());
    CHECK(int_val.is<int64_t>());
    CHECK(!int_val.is<std::string>());
    
    Value str_val("test");
    CHECK(str_val.is<std::string>());
    
    Value bool_val(true);
    CHECK(bool_val.is<bool>());
    
    Value null_val;
    CHECK(null_val.is<std::nullptr_t>());
}

TEST(Value, Truthy) {
    Value int_zero(0);
    Value int_pos(42);
    CHECK(!int_zero.is_truthy());
    CHECK(int_pos.is_truthy());
    
    Value str_empty("");
    Value str_non_empty("hello");
    CHECK(!str_empty.is_truthy());
    CHECK(str_non_empty.is_truthy());
    
    Value bool_false(false);
    Value bool_true(true);
    CHECK(!bool_false.is_truthy());
    CHECK(bool_true.is_truthy());
}