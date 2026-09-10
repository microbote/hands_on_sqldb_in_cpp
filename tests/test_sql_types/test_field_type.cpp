// test_field_type.cpp
#include "test_framework.h"
#include "sql_types/field_type.h"

using namespace sql;

TEST(FieldType, BasicTypes) {
    DataType int_type = DataType::INT;
    CHECK(is_integer(int_type));
    CHECK(!is_string(int_type));
    CHECK(!is_boolean(int_type));
    
    DataType varchar_type = DataType::VARCHAR;
    CHECK(!is_integer(varchar_type));
    CHECK(is_string(varchar_type));
    CHECK(!is_boolean(varchar_type));
    
    DataType bool_type = DataType::BOOLEAN;
    CHECK(!is_integer(bool_type));
    CHECK(!is_string(bool_type));
    CHECK(is_boolean(bool_type));
}

TEST(FieldType, Size) {
    CHECK_EQ(sizeof(DataType::INT), sizeof(uint8_t));
    CHECK_EQ(sizeof(DataType::BIGINT), sizeof(uint8_t));
    CHECK_EQ(sizeof(DataType::BOOLEAN), sizeof(uint8_t));
    
    
    // VARCHAR 等变长类型
    // CHECK_EQ(size_of(DataType::VARCHAR), 0);  // 或特定值
}

TEST(FieldType, NameConversion) {
    // 类型 -> 名称
    // 注意：data_type_name 返回 const char*，用 CHECK_EQ 会退化成指针比较
    CHECK_STREQ(data_type_name(DataType::INT), "INT");
    CHECK_STREQ(data_type_name(DataType::VARCHAR), "VARCHAR");
    CHECK_STREQ(data_type_name(DataType::BOOLEAN), "BOOLEAN");
    
    // 名称 -> 类型
    CHECK(string_to_data_type("INT") == DataType::INT);
    CHECK(string_to_data_type("INTEGER") == DataType::INT);
    CHECK(string_to_data_type("VARCHAR") == DataType::VARCHAR);
    CHECK(string_to_data_type("BOOLEAN") == DataType::BOOLEAN);
    CHECK(string_to_data_type("BOOL") == DataType::BOOLEAN);
    
    // 大小写不敏感
    CHECK(string_to_data_type("int") == DataType::INT);
    CHECK(string_to_data_type("Varchar") == DataType::VARCHAR);
    CHECK(string_to_data_type("boolean") == DataType::BOOLEAN);
}

TEST(FieldType, SupportFunctions) {
    // 是否可以比较/排序
    CHECK(is_orderable(DataType::INT));
    CHECK(is_orderable(DataType::VARCHAR));
    CHECK(is_indexable(DataType::INT));
    
    // 是否可哈希
    CHECK(is_hashable(DataType::INT));
}

TEST(FieldType, StringToDataType) {
    // 基本类型
    CHECK(string_to_data_type("INT") == DataType::INT);
    CHECK(string_to_data_type("BIGINT") == DataType::BIGINT);
    CHECK(string_to_data_type("VARCHAR") == DataType::VARCHAR);
    CHECK(string_to_data_type("TEXT") == DataType::TEXT);
    CHECK(string_to_data_type("BOOLEAN") == DataType::BOOLEAN);
    
    // 大小写不敏感
    CHECK(string_to_data_type("int") == DataType::INT);
    CHECK(string_to_data_type("Int") == DataType::INT);
    CHECK(string_to_data_type("Varchar") == DataType::VARCHAR);
    CHECK(string_to_data_type("boolean") == DataType::BOOLEAN);
    
    // 别名
    CHECK(string_to_data_type("INTEGER") == DataType::INT);
    CHECK(string_to_data_type("BOOL") == DataType::BOOLEAN);
    //CHECK(string_to_data_type("CHAR") == DataType::VARCHAR);
    
    // NULL
    CHECK(string_to_data_type("NULL") == DataType::NULL_TYPE);
    
    // 未知类型
    CHECK(string_to_data_type("FLOAT") == DataType::UNKNOWN_TYPE);
    CHECK(string_to_data_type("DATE") == DataType::UNKNOWN_TYPE);
    CHECK(string_to_data_type("") == DataType::UNKNOWN_TYPE);
}

TEST(FieldType, RoundTrip) {
    // data_type_name 和 string_to_data_type 互逆
    CHECK(string_to_data_type(data_type_name(DataType::INT)) == DataType::INT);
    CHECK(string_to_data_type(data_type_name(DataType::BIGINT)) == DataType::BIGINT);
    CHECK(string_to_data_type(data_type_name(DataType::VARCHAR)) == DataType::VARCHAR);
    CHECK(string_to_data_type(data_type_name(DataType::TEXT)) == DataType::TEXT);
    CHECK(string_to_data_type(data_type_name(DataType::BOOLEAN)) == DataType::BOOLEAN);
}
