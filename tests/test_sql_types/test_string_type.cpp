// tests/test_sql_types/test_string_type.cpp
//
// 字符串 family 按整型 family 的同一套约定处理：
//   - 声明长度只存在于 schema（ColumnDef::length），进校验、不进存储/key；
//   - CHAR/VARCHAR/TEXT 共用同一个 key 族，NULL key 也相同；
//   - 写入与读取都按字节数做容量校验。
#include "test_framework.h"
#include "sql_types/key.h"
#include "sql_types/key_range.h"
#include "sql_types/row.h"
#include "sql_types/schema.h"
#include "sql_types/sql_truth.h"

using namespace sql;

// ============================================================
// 类型解析与容量元数据
// ============================================================

TEST(StringType, ParseTypeWithDeclaredLength) {
    DataType type = DataType::UNKNOWN_TYPE;
    uint32_t length = 0;

    CHECK(parse_type_with_length("VARCHAR(32)", type, length));
    CHECK(type == DataType::VARCHAR);
    CHECK_EQ(length, 32u);

    CHECK(parse_type_with_length("varchar(255)", type, length));
    CHECK(type == DataType::VARCHAR);
    CHECK_EQ(length, 255u);

    CHECK(parse_type_with_length("CHAR(4)", type, length));
    CHECK(type == DataType::CHAR);
    CHECK_EQ(length, 4u);

    // 不带长度
    CHECK(parse_type_with_length("TEXT", type, length));
    CHECK(type == DataType::TEXT);
    CHECK_EQ(length, 0u);

    CHECK(parse_type_with_length("VARCHAR", type, length));
    CHECK(type == DataType::VARCHAR);
    CHECK_EQ(length, 0u);

    // 非字符串类型不允许带参数
    CHECK(parse_type_with_length("INT", type, length));
    CHECK(type == DataType::INT);
    CHECK_EQ(length, 0u);
    CHECK(!parse_type_with_length("INT(4)", type, length));

    // 非法：0、越界、TEXT 带长度、未知类型、括号不完整
    CHECK(!parse_type_with_length("VARCHAR(0)", type, length));
    CHECK(!parse_type_with_length("VARCHAR(65536)", type, length));
    CHECK(!parse_type_with_length("CHAR(256)", type, length));
    CHECK(!parse_type_with_length("TEXT(10)", type, length));
    CHECK(!parse_type_with_length("BLOB(10)", type, length));
    CHECK(!parse_type_with_length("VARCHAR(10", type, length));
    CHECK(!parse_type_with_length("", type, length));
}

TEST(StringType, CapacityMetadata) {
    // 未声明长度 -> 类型默认容量
    CHECK_EQ(string_capacity(DataType::CHAR, 0), kMaxCharLength);
    CHECK_EQ(string_capacity(DataType::VARCHAR, 0), kMaxVarcharLength);
    CHECK_EQ(string_capacity(DataType::TEXT, 0), kMaxTextLength);

    // 声明长度 -> 用声明值
    CHECK_EQ(string_capacity(DataType::CHAR, 10), 10u);
    CHECK_EQ(string_capacity(DataType::VARCHAR, 32), 32u);
    // TEXT 忽略声明值，容量恒定
    CHECK_EQ(string_capacity(DataType::TEXT, 10), kMaxTextLength);

    // 0 表示未声明，任何类型都合法
    CHECK(valid_declared_length(DataType::CHAR, 0));
    CHECK(valid_declared_length(DataType::VARCHAR, 0));
    CHECK(valid_declared_length(DataType::TEXT, 0));
    CHECK(valid_declared_length(DataType::INT, 0));

    // 显式声明的边界
    CHECK(valid_declared_length(DataType::CHAR, 1));
    CHECK(valid_declared_length(DataType::CHAR, 255));
    CHECK(!valid_declared_length(DataType::CHAR, 256));
    CHECK(valid_declared_length(DataType::VARCHAR, 65535));
    CHECK(!valid_declared_length(DataType::VARCHAR, 65536));
    CHECK(!valid_declared_length(DataType::TEXT, 10));
    CHECK(!valid_declared_length(DataType::INT, 4));
}

TEST(StringType, CapacityCheckIsByteBased) {
    CHECK(can_represent_length(DataType::VARCHAR, 5, 5));
    CHECK(!can_represent_length(DataType::VARCHAR, 5, 6));
    CHECK(can_represent_length(DataType::TEXT, 0, kMaxTextLength));
    CHECK(!can_represent_length(DataType::TEXT, 0, kMaxTextLength + 1));
    CHECK(can_represent_length(DataType::CHAR, 3, 3));
    CHECK(!can_represent_length(DataType::CHAR, 3, 4));
}

TEST(StringType, FamilyAndPromotion) {
    CHECK(is_string(DataType::CHAR));
    CHECK(is_string(DataType::VARCHAR));
    CHECK(is_string(DataType::TEXT));
    CHECK(is_same_family(DataType::CHAR, DataType::TEXT));
    CHECK(is_same_family(DataType::VARCHAR, DataType::CHAR));
    CHECK(!is_same_family(DataType::CHAR, DataType::INT));

    // 与整型一样提升到"更宽"的类型：CHAR -> VARCHAR -> TEXT
    CHECK(common_type(DataType::CHAR, DataType::CHAR) == DataType::CHAR);
    CHECK(common_type(DataType::CHAR, DataType::VARCHAR) == DataType::VARCHAR);
    CHECK(common_type(DataType::CHAR, DataType::TEXT) == DataType::TEXT);
    CHECK(common_type(DataType::VARCHAR, DataType::TEXT) == DataType::TEXT);
    CHECK(common_type(DataType::VARCHAR, DataType::INT) ==
          DataType::UNKNOWN_TYPE);
}

TEST(StringType, CommonCapacityTakesTheWiderSide) {
    // 与整型"提升到更宽类型"对应：结果容量取较大者
    CHECK_EQ(common_string_capacity(DataType::VARCHAR, 5, DataType::VARCHAR, 50),
             50u);
    CHECK_EQ(common_string_capacity(DataType::CHAR, 4, DataType::VARCHAR, 10),
             10u);
    // 未声明长度 -> 用默认容量
    CHECK_EQ(common_string_capacity(DataType::CHAR, 4, DataType::VARCHAR, 0),
             kMaxVarcharLength);
    // TEXT 参与时结果是 TEXT 容量
    CHECK_EQ(common_string_capacity(DataType::VARCHAR, 10, DataType::TEXT, 0),
             kMaxTextLength);
    CHECK_EQ(common_string_capacity(DataType::CHAR, 0, DataType::CHAR, 0),
             kMaxCharLength);
}

// ============================================================
// 写入 / 读取的范围校验
// ============================================================

namespace {

TableSchema make_string_schema(DataType type, uint32_t length) {
    TableSchema schema;
    schema.set_name(Identifier("t"));
    schema.add_column(Identifier("id"), DataType::INT, true, false);
    schema.add_column(Identifier("s"), type, length, false, true);
    return schema;
}

}  // namespace

TEST(StringType, SchemaRejectsTooLongValue) {
    auto schema = make_string_schema(DataType::VARCHAR, 5);
    CHECK_EQ(schema.validate(), SchemaError::OK);

    // 边界：正好 5 字节可以写
    CHECK(row()
              .set(Identifier("id"), 1)
              .set(Identifier("s"), std::string("abcde"))
              .build(schema)
              .has_value());

    auto too_long = row()
                        .set(Identifier("id"), 1)
                        .set(Identifier("s"), std::string("abcdef"))
                        .build(schema);
    CHECK(!too_long.has_value());
    CHECK(too_long.error() == SchemaError::VALUE_OUT_OF_RANGE);

    // TEXT 的容量边界（65535 字节）
    auto text_schema = make_string_schema(DataType::TEXT, 0);
    CHECK(row()
              .set(Identifier("id"), 1)
              .set(Identifier("s"), std::string(kMaxTextLength, 'x'))
              .build(text_schema)
              .has_value());
    auto text_too_long = row()
                             .set(Identifier("id"), 1)
                             .set(Identifier("s"),
                                  std::string(kMaxTextLength + 1, 'x'))
                             .build(text_schema);
    CHECK(!text_too_long.has_value());
    CHECK(text_too_long.error() == SchemaError::VALUE_OUT_OF_RANGE);
}

TEST(StringType, CharIsLimitedTo255Bytes) {
    auto schema = make_string_schema(DataType::CHAR, 3);
    CHECK(row()
              .set(Identifier("id"), 1)
              .set(Identifier("s"), std::string("abc"))
              .build(schema)
              .has_value());

    auto too_long = row()
                        .set(Identifier("id"), 1)
                        .set(Identifier("s"), std::string("abcd"))
                        .build(schema);
    CHECK(!too_long.has_value());
    CHECK(too_long.error() == SchemaError::VALUE_OUT_OF_RANGE);
}

TEST(StringType, LengthIsCountedInBytesNotCharacters) {
    // "中文" 是 6 字节（UTF-8 每字 3 字节）
    const std::string chinese = "中文";
    CHECK_EQ(chinese.size(), 6u);

    auto schema_5 = make_string_schema(DataType::VARCHAR, 5);
    auto too_long = row()
                        .set(Identifier("id"), 1)
                        .set(Identifier("s"), chinese)
                        .build(schema_5);
    CHECK(!too_long.has_value());
    CHECK(too_long.error() == SchemaError::VALUE_OUT_OF_RANGE);

    auto schema_6 = make_string_schema(DataType::VARCHAR, 6);
    CHECK(row()
              .set(Identifier("id"), 1)
              .set(Identifier("s"), chinese)
              .build(schema_6)
              .has_value());
}

TEST(StringType, OverLongStoredValueIsRejectedOnRead) {
    // 用较宽的 schema 写入，再用较窄的 schema 读取（模拟数据被改写、
    // 或 schema 收窄后的旧数据）：key 合法但内容超出新声明长度
    auto wide = make_string_schema(DataType::VARCHAR, 10);
    auto narrow = make_string_schema(DataType::VARCHAR, 3);

    auto built = row()
                     .set(Identifier("id"), 1)
                     .set(Identifier("s"), std::string("abcde"))
                     .build(wide);
    CHECK(built.has_value());
    if (!built.has_value()) {
        return;
    }

    const std::string data = built->serialize(wide);
    auto restored = Row::deserialize(data, narrow);
    CHECK(!restored.has_value());
    CHECK(restored.error() == SchemaError::VALUE_OUT_OF_RANGE);

    // 长度在声明范围内的数据可以正常读回
    auto ok_built = row()
                        .set(Identifier("id"), 1)
                        .set(Identifier("s"), std::string("abc"))
                        .build(wide);
    CHECK(ok_built.has_value());
    if (ok_built.has_value()) {
        auto ok = Row::deserialize(ok_built->serialize(wide), narrow);
        CHECK(ok.has_value());
        if (ok.has_value()) {
            CHECK_EQ((*ok)[1].as_str(), std::string("abc"));
        }
    }
}

TEST(StringType, InvalidDeclaredLengthIsRejectedAtDdlTime) {
    TableSchema schema;
    schema.set_name(Identifier("t"));
    schema.add_column(Identifier("bad_char"), DataType::CHAR, 300u, false, true);
    CHECK(schema.has_error());
    CHECK(schema.validate() == SchemaError::INVALID_COLUMN_DEF);

    TableSchema schema2;
    schema2.set_name(Identifier("t"));
    schema2.add_column(Identifier("bad_text"), DataType::TEXT, 10u, false, true);
    CHECK(schema2.validate() == SchemaError::INVALID_COLUMN_DEF);

    TableSchema schema3;
    schema3.set_name(Identifier("t"));
    schema3.add_column(Identifier("bad_int"), DataType::INT, 4u, false, true);
    CHECK(schema3.validate() == SchemaError::INVALID_COLUMN_DEF);
}

// ============================================================
// 存储一致性：声明长度不进 key
// ============================================================

TEST(StringType, DeclaredLengthIsNotPartOfTheKey) {
    const Value value(std::string("abc"));

    // 与整型 family 一样：族内所有类型共用同一编码
    CHECK_EQ(value.to_key(DataType::CHAR), value.to_key(DataType::VARCHAR));
    CHECK_EQ(value.to_key(DataType::VARCHAR), value.to_key(DataType::TEXT));
    CHECK_EQ(KeyCodecs::tag_of(DataType::CHAR),
             KeyCodecs::tag_of(DataType::VARCHAR));
    CHECK_EQ(KeyCodecs::tag_of(DataType::CHAR), KeyCodecs::kTagString);

    // NULL 的 key 也相同（= 字符串族的族下界）
    const Value null_value;
    CHECK_EQ(null_value.to_key(DataType::CHAR),
             null_value.to_key(DataType::VARCHAR));
    CHECK_EQ(null_value.to_key(DataType::CHAR),
             null_value.to_key(DataType::TEXT));
    CHECK_EQ(null_value.to_key(DataType::CHAR),
             Value::min_key_for_type(DataType::CHAR));

    // 族扫描仍然覆盖 CHAR/VARCHAR/TEXT 的全部值
    const auto range = KeyRange::all(DataType::TEXT).to_str_key_range();
    const Key k = value.to_key(DataType::CHAR);
    CHECK(range.start <= k);
    CHECK(k < range.end);
}

TEST(StringType, TwoColumnsWithDifferentLengthsShareEncoding) {
    TableSchema schema;
    schema.set_name(Identifier("t"));
    schema.add_column(Identifier("id"), DataType::INT, true, false);
    schema.add_column(Identifier("a"), DataType::VARCHAR, 5u, false, true);
    schema.add_column(Identifier("b"), DataType::VARCHAR, 50u, false, true);
    CHECK_EQ(schema.validate(), SchemaError::OK);

    auto built = row()
                     .set(Identifier("id"), 1)
                     .set(Identifier("a"), std::string("abc"))
                     .set(Identifier("b"), std::string("abc"))
                     .build(schema);
    CHECK(built.has_value());
    if (built.has_value()) {
        // 同样的值在两列里编码完全相同
        CHECK_EQ((*built)[1].to_key(DataType::VARCHAR),
                 (*built)[2].to_key(DataType::VARCHAR));
    }
}

// ============================================================
// 序列化与展示
// ============================================================

TEST(StringType, SchemaSerDesKeepsDeclaredLength) {
    TableSchema schema;
    schema.set_name(Identifier("t"));
    schema.add_column(Identifier("id"), DataType::INT, true, false);
    schema.add_column(Identifier("code"), DataType::CHAR, 8u, false, true);
    schema.add_column(Identifier("name"), DataType::VARCHAR, 32u, false, true);
    schema.add_column(Identifier("body"), DataType::TEXT, false, true);

    auto restored = TableSchema::deserialize(schema.serialize());
    CHECK(restored.has_value());
    if (restored.has_value()) {
        CHECK(*restored == schema);
        CHECK_EQ(restored->column_at(1)->length, 8u);
        CHECK_EQ(restored->column_at(2)->length, 32u);
        CHECK_EQ(restored->column_at(3)->length, 0u);
        // 反序列化后校验行为一致
        auto too_long = row()
                            .set(Identifier("id"), 1)
                            .set(Identifier("name"), std::string(33, 'x'))
                            .build(*restored);
        CHECK(!too_long.has_value());
        CHECK(too_long.error() == SchemaError::VALUE_OUT_OF_RANGE);
    }
}

TEST(StringType, TypeDisplayShowsDeclaredLength) {
    const ColumnDef char_col(Identifier("c"), DataType::CHAR, 8u);
    const ColumnDef varchar_col(Identifier("v"), DataType::VARCHAR, 32u);
    const ColumnDef text_col(Identifier("t"), DataType::TEXT);
    const ColumnDef int_col(Identifier("i"), DataType::INT);

    CHECK_EQ(char_col.type_display(), std::string("CHAR(8)"));
    CHECK_EQ(varchar_col.type_display(), std::string("VARCHAR(32)"));
    CHECK_EQ(text_col.type_display(), std::string("TEXT"));
    CHECK_EQ(int_col.type_display(), std::string("INT"));

    TableSchema schema;
    schema.set_name(Identifier("t"));
    schema.add_column(varchar_col);
    CHECK(schema.to_string().find("VARCHAR(32)") != std::string::npos);
}

// ============================================================
// 比较语义不受声明长度影响
// ============================================================

TEST(StringType, ComparisonNeverTruncates) {
    // 声明长度只是约束，不参与比较：比较按完整字节序
    CHECK(sql_compare_op(CompareOp::LT, Value(std::string("abc")),
                         Value(std::string("abcd"))) == Truth::TRUE);
    CHECK(sql_compare_op(CompareOp::EQ, Value(std::string("abc")),
                         Value(std::string("abc"))) == Truth::TRUE);

    // 说明：CHAR 目前不做空格填充（MySQL 的 PAD SPACE collation 会认为
    // "abc" = "abc "），这里是二进制比较的已知差异
    CHECK(sql_compare_op(CompareOp::EQ, Value(std::string("abc")),
                         Value(std::string("abc "))) == Truth::FALSE);
}
