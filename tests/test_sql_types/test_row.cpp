// tests/test_sql_types/test_row.cpp
#include "test_framework.h"
#include "sql_types/row.h"
#include "sql_types/field_type.h"
#include "sql_types/schema.h"

using namespace sql;

TEST(Row, DefaultConstructor) {
    Row row;
    CHECK(row.is_empty());
    CHECK_EQ(row.size(), 0);
}

TEST(Row, PushBackValues) {
    Row row;
    row.push_back(Value(42));
    row.push_back(Value(std::string("hello")));
    row.push_back(Value(true));
    
    CHECK_EQ(row.size(), 3);
    CHECK(!row.is_empty());
    CHECK_EQ(row[0].as_int(), 42);
    CHECK_EQ(row[1].as_str(), "hello");
    CHECK_EQ(row[2].as_bool(), true);
}

TEST(Row, SetAndGet) {
    Row row;
    row.push_back(Value(42));
    row.push_back(Value(std::string("hello")));
    
    row[0] = Value(100);
    row[1] = Value(std::string("world"));
    
    CHECK_EQ(row[0].as_int(), 100);
    CHECK_EQ(row[1].as_str(), "world");
}

TEST(Row, NullValues) {
    Row row;
    row.push_back(Value());  // NULL
    CHECK(row[0].is_null());
}

TEST(Row, CopyAndMove) {
    Row original;
    original.push_back(Value(1));
    original.push_back(Value(std::string("test")));
    
    Row copy = original;
    CHECK_EQ(copy.size(), 2);
    CHECK_EQ(copy[0].as_int(), 1);
    
    // 修改副本不影响原
    copy[0] = Value(100);
    CHECK_EQ(original[0].as_int(), 1);
    
    Row moved = std::move(copy);
    CHECK_EQ(moved.size(), 2);
    CHECK_EQ(moved[0].as_int(), 100);
}

TEST(Row, Iteration) {
    Row row;
    row.push_back(Value(1));
    row.push_back(Value(2));
    row.push_back(Value(3));
    
    int sum = 0;
    for (const auto& v : row) {
        sum += v.as_int();
    }
    CHECK_EQ(sum, 6);
}

// ============================================================
// RowBuilder 测试
// ============================================================

// 构建测试用的 schema
TableSchema create_test_schema() {
    TableSchema schema;
    schema.add_column(Identifier("id"), DataType::INT, true, false);   // PK, NOT NULL
    schema.add_column(Identifier("name"), DataType::VARCHAR, false, true); // nullable
    schema.add_column(Identifier("age"), DataType::INT, false, true);  // nullable
    schema.add_column(Identifier("active"), DataType::BOOLEAN, false, true); // nullable
    return schema;
}

TEST(RowBuilder, BuildValidRow) {
    auto schema = create_test_schema();
    CHECK_EQ(schema.validate(), SchemaError::OK);  // schema 应有效
    
    auto result = row()
        .set(Identifier("id"), 1)                    // int
        .set(Identifier("name"), "Alice")            // const char*
        .set(Identifier("age"), 30)                  // int
        .set_bool(Identifier("active"), true)        // bool
        .build(schema);
    
    CHECK(result.has_value());  // 构建成功
    
    if (result.has_value()) {
        Row& r = *result;
        CHECK_EQ(r.size(), 4);  // 4 列
        CHECK_EQ(r[0].as_int(), 1);
        CHECK_EQ(r[1].as_str(), "Alice");
        CHECK_EQ(r[2].as_int(), 30);
        CHECK_EQ(r[3].as_bool(), true);
    }
}

TEST(RowBuilder, BuildWithStringAndNull) {
    auto schema = create_test_schema();
    
    auto result = row()
        .set(Identifier("id"), 2)
        .set(Identifier("name"), std::string("Bob"))  // std::string
        .set_null(Identifier("age"))                   // NULL
        .set_bool(Identifier("active"), false)
        .build(schema);
    
    CHECK(result.has_value());
    
    if (result.has_value()) {
        Row& r = *result;
        CHECK_EQ(r[0].as_int(), 2);
        CHECK_EQ(r[1].as_str(), "Bob");
        CHECK(r[2].is_null());  // age 为 NULL（schema 允许 nullable）
        CHECK_EQ(r[3].as_bool(), false);
    }
}

TEST(RowBuilder, MissingRequiredColumn) {
    auto schema = create_test_schema();
    
    // 缺少 PRIMARY KEY "id"
    auto result = row()
        .set(Identifier("name"), "Alice")
        .set(Identifier("age"), 30)
        .build(schema);
    
    CHECK(!result.has_value());  // 应失败
    CHECK(result.error() == SchemaError::INVALID_ROW);
}

TEST(RowBuilder, ColumnNotFound) {
    auto schema = create_test_schema();
    
    // 尝试设置 schema 中不存在的列
    auto result = row()
        .set(Identifier("id"), 1)
        .set(Identifier("nonexistent"), "value")
        .build(schema);
    
    CHECK(!result.has_value());
    CHECK(result.error() == SchemaError::COLUMN_NOT_FOUND);
}

TEST(RowBuilder, DuplicateColumns) {
    auto schema = create_test_schema();
    
    auto result = row()
        .set(Identifier("id"), 1)
        .set(Identifier("name"), "Alice")
        .set(Identifier("name"), "Bob")  // 重复设置
        .build(schema);
    
    // 最后设置的值应覆盖之前的（合理行为）
    CHECK(result.has_value());
    if (result.has_value()) {
        CHECK_EQ((*result)[1].as_str(), "Bob");
    }
}

TEST(RowBuilder, BuildOrdered) {
    auto schema = create_test_schema();
    
    // 指定列的插入顺序
    std::vector<Identifier> order = {
        Identifier("name"),
        Identifier("id"),
        Identifier("active"),
        Identifier("age")
    };
    
    auto result = row()
        .set(Identifier("id"), 10)
        .set(Identifier("name"), "Charlie")
        .set_bool(Identifier("active"), true)
        .set(Identifier("age"), 25)
        .build_ordered(schema, order);
    
    CHECK(result.has_value());
    
    if (result.has_value()) {
        Row& r = *result;
        CHECK_EQ(r.size(), 4);
        // 按照指定的 order 排列
        CHECK_EQ(r[0].as_str(), "Charlie");  // name
        CHECK_EQ(r[1].as_int(), 10);         // id
        CHECK_EQ(r[2].as_bool(), true);      // active
        CHECK_EQ(r[3].as_int(), 25);         // age
    }
}

TEST(RowBuilder, ContainsAndGet) {
    auto builder = row()
        .set(Identifier("id"), 1)
        .set(Identifier("name"), "Alice");
    
    CHECK(builder.has(Identifier("id")));
    CHECK(builder.has(Identifier("name")));
    CHECK(!builder.has(Identifier("age")));  // 未设置
    
    Value id_val = builder.get(Identifier("id"));
    CHECK_EQ(id_val.as_int(), 1);
    
    Value missing = builder.get(Identifier("age"));
    CHECK(missing.is_null());  // 默认为 NULL
}

TEST(RowBuilder, SchemaSerializationWithRow) {
    auto schema = create_test_schema();
    CHECK_EQ(schema.validate(), SchemaError::OK);
    
    // 构建行
    auto result = row()
        .set(Identifier("id"), 42)
        .set(Identifier("name"), "Test")
        .set(Identifier("age"), 99)
        .set_bool(Identifier("active"), false)
        .build(schema);
    
    CHECK(result.has_value());
    
    if (result.has_value()) {
        Row& r = *result;
        
        // 序列化行（不带 schema 信息）
        std::string data = r.serialize();
        CHECK(!data.empty());
        
        // 反序列化需要 schema
        Row restored = Row::deserialize(data, schema);
        CHECK_EQ(restored.size(), r.size());
        CHECK_EQ(restored[0].as_int(), 42);
        CHECK_EQ(restored[1].as_str(), "Test");
        CHECK_EQ(restored[2].as_int(), 99);
        CHECK_EQ(restored[3].as_bool(), false);
        CHECK(restored == r);
    }
}