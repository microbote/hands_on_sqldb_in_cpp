// tests/test_sql_types/test_serialize.cpp
//
// 序列化是"落盘契约"，这里覆盖长度前缀 framing 的边界：
// 分隔符字符、空串、NULL、二进制字节、版本号、损坏数据。
#include "test_framework.h"
#include "sql_types/row.h"
#include "sql_types/schema.h"

using namespace sql;

namespace {

TableSchema make_schema() {
    TableSchema s;
    s.set_name(Identifier("t"));
    s.add_column(Identifier("id"), DataType::INT, true, false);
    s.add_column(Identifier("name"), DataType::VARCHAR, false, true);
    s.add_column(Identifier("bio"), DataType::TEXT, false, true);
    s.add_column(Identifier("flag"), DataType::BOOLEAN, false, true);
    return s;
}

}  // namespace

TEST(Serialize, RowRoundTripWithSeparatorsInData) {
    auto schema = make_schema();

    Row row;
    row.push_back(Value(1));
    row.push_back(Value(std::string("a|b,c:d")));   // 旧格式会被这些字符击穿
    row.push_back(Value::text(std::string("x|y")));
    row.push_back(Value(true));

    auto restored = Row::deserialize(row.serialize(), schema);
    CHECK(restored.has_value());
    if (restored.has_value()) {
        CHECK(*restored == row);
        CHECK_EQ((*restored)[1].as_str(), "a|b,c:d");
        CHECK_EQ((*restored)[2].as_str(), "x|y");
    }
}

TEST(Serialize, RowRoundTripWithBinaryAndEmptyStrings) {
    auto schema = make_schema();

    Row row;
    row.push_back(Value(7));
    row.push_back(Value(std::string("")));              // 空串 != NULL
    row.push_back(Value::text(std::string("a\0b", 3))); // 内嵌 0x00
    row.push_back(Value(false));

    auto restored = Row::deserialize(row.serialize(), schema);
    CHECK(restored.has_value());
    if (restored.has_value()) {
        CHECK(!(*restored)[1].is_null());
        CHECK_EQ((*restored)[1].as_str().size(), 0);
        CHECK_EQ((*restored)[2].as_str(), std::string("a\0b", 3));
        CHECK_EQ((*restored)[3].as_bool(), false);
    }
}

TEST(Serialize, RowRoundTripWithNulls) {
    auto schema = make_schema();

    Row row;
    row.push_back(Value(3));
    row.push_back(Value());    // NULL
    row.push_back(Value());    // NULL
    row.push_back(Value());    // NULL

    auto restored = Row::deserialize(row.serialize(), schema);
    CHECK(restored.has_value());
    if (restored.has_value()) {
        CHECK((*restored)[1].is_null());
        CHECK((*restored)[2].is_null());
        CHECK((*restored)[3].is_null());
        CHECK(*restored == row);
    }
}

TEST(Serialize, RowRejectsCorruptionAndVersionMismatch) {
    auto schema = make_schema();
    Row row;
    row.push_back(Value(1));
    row.push_back(Value(std::string("x")));
    row.push_back(Value::text(std::string("y")));
    row.push_back(Value(true));
    std::string data = row.serialize();

    // 空数据 / 截断
    CHECK(!Row::deserialize("", schema).has_value());
    CHECK(!Row::deserialize(data.substr(0, data.size() - 1), schema).has_value());

    // 版本号不对
    std::string bad_version = data;
    bad_version[0] = static_cast<char>(Row::kFormatVersion + 1);
    auto r1 = Row::deserialize(bad_version, schema);
    CHECK(!r1.has_value());
    CHECK(r1.error() == SchemaError::INVALID_FORMAT);

    // 字段数不匹配
    std::string bad_count = data;
    bad_count[4] = static_cast<char>(99);
    auto r2 = Row::deserialize(bad_count, schema);
    CHECK(!r2.has_value());
    CHECK(r2.error() == SchemaError::COLUMN_SIZE_MISMATCH);

    // 尾部多出垃圾字节
    CHECK(!Row::deserialize(data + "junk", schema).has_value());

    // 字段内容不是该列的合法 key
    std::string bad_key = data;
    bad_key[9] = 'X';   // 破坏第一个字段的族标记
    CHECK(!Row::deserialize(bad_key, schema).has_value());
}

TEST(Serialize, RowRejectsWrongColumnCount) {
    auto schema = make_schema();
    Row short_row;
    short_row.push_back(Value(1));

    auto r = Row::deserialize(short_row.serialize(), schema);
    CHECK(!r.has_value());
    CHECK(r.error() == SchemaError::COLUMN_SIZE_MISMATCH);
}

TEST(Serialize, SchemaRoundTrip) {
    auto schema = make_schema();
    auto restored = TableSchema::deserialize(schema.serialize());

    CHECK(restored.has_value());
    if (restored.has_value()) {
        CHECK(*restored == schema);
        CHECK_EQ(restored->primary_key_index(), schema.primary_key_index());
    }
}

TEST(Serialize, SchemaHandlesSeparatorsInIdentifiers) {
    TableSchema schema;
    schema.set_name(Identifier("weird|name,with:chars"));
    schema.add_column(ColumnDef{Identifier("id|x"), DataType::INT, true, false});
    schema.add_column(
        ColumnDef{Identifier("a,b:c"), DataType::VARCHAR, false, true});

    auto restored = TableSchema::deserialize(schema.serialize());
    CHECK(restored.has_value());
    if (restored.has_value()) {
        CHECK(restored->table_name() == schema.table_name());
        CHECK(restored->column_at(0)->name == Identifier("id|x"));
        CHECK(restored->column_at(1)->name == Identifier("a,b:c"));
    }
}

TEST(Serialize, SchemaRejectsCorruptionAndVersionMismatch) {
    auto schema = make_schema();
    std::string data = schema.serialize();

    CHECK(!TableSchema::deserialize("").has_value());
    CHECK(!TableSchema::deserialize(data.substr(0, data.size() - 1)).has_value());

    std::string bad_version = data;
    bad_version[0] = static_cast<char>(TableSchema::kFormatVersion + 1);
    auto r = TableSchema::deserialize(bad_version);
    CHECK(!r.has_value());
    CHECK(r.error() == SchemaError::INVALID_FORMAT);

    std::string trailing = data + "extra";
    CHECK(!TableSchema::deserialize(trailing).has_value());
}
