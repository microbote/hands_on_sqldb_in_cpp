// test_schema.cpp
#include "test_framework.h"
#include "sql_types/schema.h"
#include "sql_types/field_type.h"

using namespace sql;

// ============================================================
// ColumnDef 测试
// ============================================================
TEST(Schema, ColumnDefBasic) {
    ColumnDef col;
    col.name = Identifier("id");
    col.type = DataType::INT;
    col.primary_key = true;
    col.nullable = false;
    
    CHECK_EQ(col.name.str(), "id");
    CHECK(col.type == DataType::INT);
    CHECK(col.primary_key);
    CHECK(!col.nullable);
}

TEST(Schema, ColumnDefToString) {
    ColumnDef col;
    col.name = Identifier("id");
    col.type = DataType::INT;
    col.primary_key = true;
    col.nullable = false;
    
    std::string s = col.to_string();
    CHECK(s.find("id") != std::string::npos);
    CHECK(s.find("INT") != std::string::npos);
    
    // 不同类型
    ColumnDef name_col;
    name_col.name = Identifier("name");
    name_col.type = DataType::VARCHAR;
    name_col.nullable = true;
    
    s = name_col.to_string();
    CHECK(s.find("name") != std::string::npos);
    CHECK(s.find("VARCHAR") != std::string::npos);
}

// ============================================================
// TableSchema 测试
// ============================================================
TEST(Schema, EmptySchema) {
    TableSchema schema;
    CHECK(schema.is_empty());
    CHECK_EQ(schema.column_count(), 0);
    CHECK_EQ(schema.has_primary_key(), false);
}

TEST(Schema, BuildBasicSchema) {
    TableSchema schema;
    schema.set_name(Identifier("users"));
    
    // 添加列
    ColumnDef id_col;
    id_col.name = Identifier("id");
    id_col.type = DataType::INT;
    id_col.primary_key = true;
    id_col.nullable = false;
    schema.add_column(id_col);
    
    ColumnDef name_col;
    name_col.name = Identifier("name");
    name_col.type = DataType::VARCHAR;
    name_col.nullable = false;
    schema.add_column(name_col);
    
    ColumnDef age_col;
    age_col.name = Identifier("age");
    age_col.type = DataType::INT;
    age_col.nullable = true;
    schema.add_column(age_col);
    
    CHECK(!schema.is_empty());
    CHECK_EQ(schema.column_count(), 3);
    CHECK_EQ(schema.primary_key_index(), 0);
    CHECK_EQ(schema.table_name_str(), "users");
}

TEST(Schema, FindColumn) {
    TableSchema schema;
    schema.set_name(Identifier("users"));
    
    ColumnDef id_col;
    id_col.name = Identifier("id");
    id_col.type = DataType::INT;
    schema.add_column(id_col);
    
    ColumnDef name_col;
    name_col.name = Identifier("name");
    name_col.type = DataType::VARCHAR;
    schema.add_column(name_col);
    
    // 精确查找
    CHECK(schema.column_index(Identifier("id"))==0);
    CHECK(schema.column_index(Identifier("name"))==1);
    CHECK(schema.column_index(Identifier("email"))==-1);
    
    // 大小写不敏感查找
    CHECK(schema.column_index(Identifier("ID"))==0);
    CHECK(schema.column_index(Identifier("Name"))==1);
    
    // 获取列信息
    auto* col = schema.column(Identifier("id"));
    CHECK(col != nullptr);
    CHECK_EQ(col->name.str(), "id");
    CHECK(col->type == DataType::INT);
}

TEST(Schema, ColumnIndex) {
    TableSchema schema;
    schema.set_name(Identifier("users"));
    
    ColumnDef id_col;
    id_col.name = Identifier("id");
    id_col.type = DataType::INT;
    schema.add_column(id_col);
    
    ColumnDef name_col;
    name_col.name = Identifier("name");
    name_col.type = DataType::VARCHAR;
    schema.add_column(name_col);
    
    CHECK_EQ(schema.column_index(Identifier("id")), 0);
    CHECK_EQ(schema.column_index(Identifier("name")), 1);
    CHECK_EQ(schema.column_index(Identifier("age")), -1);  // 不存在
    
    // 大小写不敏感
    CHECK_EQ(schema.column_index(Identifier("ID")), 0);
}

TEST(Schema, PrimaryKey) {
    TableSchema schema;
    
    // 单列主键
    ColumnDef id_col;
    id_col.name = Identifier("id");
    id_col.type = DataType::INT;
    id_col.primary_key = true;
    schema.add_column(id_col);
    
    ColumnDef name_col;
    name_col.name = Identifier("name");
    name_col.type = DataType::VARCHAR;
    schema.add_column(name_col);
    
    CHECK_EQ(schema.has_primary_key(), true);
    
    auto* pk_col = schema.primary_key_column();
    CHECK(pk_col != nullptr);
    CHECK_EQ(pk_col->name.str(), "id");
    
}

TEST(Schema, CompositePrimaryKey) {
    TableSchema schema;
    
    ColumnDef order_id;
    order_id.name = Identifier("order_id");
    order_id.type = DataType::INT;
    order_id.primary_key = true;
    schema.add_column(order_id);
    
    ColumnDef item_id;
    item_id.name = Identifier("item_id");
    item_id.type = DataType::INT;
    item_id.primary_key = true;
    schema.add_column(item_id);
    
    ColumnDef qty;
    qty.name = Identifier("quantity");
    qty.type = DataType::INT;
    schema.add_column(qty);
    
    auto err = schema.validate();
    CHECK_EQ(err, SchemaError::DUPLICATE_PRIMARY_KEY);
    
}

TEST(Schema, ColumnTypeLookup) {
    TableSchema schema;
    
    ColumnDef id_col;
    id_col.name = Identifier("id");
    id_col.type = DataType::BIGINT;
    schema.add_column(id_col);
    
    ColumnDef name_col;
    name_col.name = Identifier("name");
    name_col.type = DataType::VARCHAR;
    schema.add_column(name_col);
    
    CHECK(schema.column_type(Identifier("id")) == DataType::BIGINT);
    CHECK(schema.column_type(Identifier("name")) == DataType::VARCHAR);
    
    // 不存在的列
    CHECK(schema.column_type(Identifier("nonexist")) == DataType::UNKNOWN_TYPE);
}

TEST(Schema, ColumnAccess) {
    TableSchema schema;
    
    ColumnDef id_col;
    id_col.name = Identifier("id");
    id_col.type = DataType::INT;
    schema.add_column(id_col);
    
    ColumnDef name_col;
    name_col.name = Identifier("name");
    name_col.type = DataType::VARCHAR;
    schema.add_column(name_col);
    
    // 按索引访问列
    const auto* col0 = schema.column_at(0);
    CHECK_EQ(col0->name.str(), "id");
    CHECK(col0->type == DataType::INT);
    
    const auto* col1 = schema.column_at(1);
    CHECK_EQ(col1->name.str(), "name");
    
    // 越界
    CHECK_THROW(schema.column_at(2));
}

TEST(Schema, ColumnNames) {
    TableSchema schema;
    schema.set_name(Identifier("users"));
    
    schema.add_column(ColumnDef{Identifier("id"), DataType::INT, true, false});
    schema.add_column(ColumnDef{Identifier("name"), DataType::VARCHAR, false, false});
    schema.add_column(ColumnDef{Identifier("age"), DataType::INT, false, true});
    
    auto names = schema.column_names();
    
    CHECK_EQ(names.size(), 3);
    CHECK_EQ(names[0], "id");
    CHECK_EQ(names[1], "name");
    CHECK_EQ(names[2], "age");
}

TEST(Schema, SerDes) {
    TableSchema schema;
    schema.set_name(Identifier("users"));
    
    schema.add_column(ColumnDef{Identifier("id"), DataType::INT, true, false});
    schema.add_column(ColumnDef{Identifier("name"), DataType::VARCHAR, false, false});
    schema.add_column(ColumnDef{Identifier("age"), DataType::INT, false, true});
    
    // 序列化
    std::string data = schema.serialize();
    CHECK(!data.empty());
    
    // 反序列化
    auto restored = TableSchema::deserialize(data);
    CHECK(restored.has_value());
    if (restored.has_value()) {
        CHECK(restored->table_name() == schema.table_name());
        CHECK_EQ(restored->column_count(), schema.column_count());

        // 逐列对比
        for (size_t i = 0; i < schema.column_count(); ++i) {
            const auto* orig = schema.column_at(i);
            const auto* copy = restored->column_at(i);
            CHECK(orig->name == copy->name);
            CHECK(orig->type == copy->type);
            CHECK_EQ(orig->primary_key, copy->primary_key);
            CHECK_EQ(orig->nullable, copy->nullable);
        }
    }
}

TEST(Schema, ToString) {
    TableSchema schema;
    schema.set_name(Identifier("users"));
    
    schema.add_column(ColumnDef{Identifier("id"), DataType::INT, true, false});
    schema.add_column(ColumnDef{Identifier("name"), DataType::VARCHAR, false, false});
    
    std::string s = schema.to_string();
    CHECK(s.find("users") != std::string::npos);
    CHECK(s.find("id") != std::string::npos);
    CHECK(s.find("name") != std::string::npos);
}

TEST(Schema, Equality) {
    TableSchema s1;
    s1.set_name(Identifier("users"));
    s1.add_column(ColumnDef{Identifier("id"), DataType::INT, true, false});
    
    TableSchema s2;
    s2.set_name(Identifier("users"));
    s2.add_column(ColumnDef{Identifier("id"), DataType::INT, true, false});
    
    CHECK(s1 == s2);
    
    // 不同表名
    TableSchema s3;
    s3.set_name(Identifier("orders"));
    s3.add_column(ColumnDef{Identifier("id"), DataType::INT, true, false});
    CHECK(s1 != s3);
    
    // 不同列
    TableSchema s4;
    s4.set_name(Identifier("users"));
    s4.add_column(ColumnDef{Identifier("name"), DataType::VARCHAR, false, false});
    CHECK(s1 != s4);
}

TEST(Schema, CopyAndMove) {
    TableSchema original;
    original.set_name(Identifier("users"));
    original.add_column(ColumnDef{Identifier("id"), DataType::INT, true, false});
    original.add_column(ColumnDef{Identifier("name"), DataType::VARCHAR, false, true});
    
    // 拷贝
    TableSchema copy = original;
    CHECK(copy == original);
    
    // 修改拷贝不影响原
    copy.set_name(Identifier("modified"));
    CHECK_EQ(original.table_name().str(), "users");
    
    // 移动
    TableSchema moved = std::move(copy);
    CHECK_EQ(moved.table_name().str(), "modified");
}
