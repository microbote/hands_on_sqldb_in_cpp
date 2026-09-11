// tests/test_sql_types/test_type_system.cpp
//
// 整型家族：逻辑宽度只在 schema/校验里，存储统一 int64；
// 类型提升与取值范围的读写校验。
#include "test_framework.h"
#include "sql_types/key.h"
#include "sql_types/row.h"
#include "sql_types/schema.h"
#include "sql_types/sql_truth.h"

using namespace sql;

TEST(TypeSystem, IntegerWidthMetadata) {
    CHECK_EQ(integer_bytes(DataType::TINYINT), 1);
    CHECK_EQ(integer_bytes(DataType::SMALLINT), 2);
    CHECK_EQ(integer_bytes(DataType::INT), 4);
    CHECK_EQ(integer_bytes(DataType::BIGINT), 8);

    CHECK_EQ(integer_min(DataType::TINYINT), -128);
    CHECK_EQ(integer_max(DataType::TINYINT), 127);
    CHECK_EQ(integer_min(DataType::SMALLINT), -32768);
    CHECK_EQ(integer_max(DataType::SMALLINT), 32767);
    CHECK_EQ(integer_min(DataType::INT), -2147483648LL);
    CHECK_EQ(integer_max(DataType::INT), 2147483647LL);
    CHECK_EQ(integer_max(DataType::BIGINT), INT64_MAX);
}

TEST(TypeSystem, CanRepresentChecksWidthsAndTemporalRanges) {
    CHECK(can_represent(DataType::TINYINT, 127));
    CHECK(!can_represent(DataType::TINYINT, 128));
    CHECK(can_represent(DataType::TINYINT, -128));
    CHECK(!can_represent(DataType::TINYINT, -129));

    CHECK(can_represent(DataType::SMALLINT, 32767));
    CHECK(!can_represent(DataType::SMALLINT, 32768));
    CHECK(can_represent(DataType::INT, 2147483647));
    CHECK(!can_represent(DataType::INT, 2147483648LL));
    CHECK(can_represent(DataType::BIGINT, INT64_MAX));

    CHECK(can_represent(DataType::TIME, 0));
    CHECK(can_represent(DataType::TIME, 86399));
    CHECK(!can_represent(DataType::TIME, 86400));
    CHECK(!can_represent(DataType::TIME, -1));
    CHECK(can_represent(DataType::DATE, kDateMinDays));
    CHECK(can_represent(DataType::DATE, kDateMaxDays));
    CHECK(!can_represent(DataType::DATE, kDateMaxDays + 1));
}

TEST(TypeSystem, SameFamilyRules) {
    CHECK(is_same_family(DataType::TINYINT, DataType::BIGINT));
    CHECK(is_same_family(DataType::INT, DataType::SMALLINT));
    CHECK(is_same_family(DataType::VARCHAR, DataType::TEXT));
    CHECK(is_same_family(DataType::DATE, DataType::DATETIME));
    CHECK(!is_same_family(DataType::INT, DataType::VARCHAR));
    CHECK(!is_same_family(DataType::DATE, DataType::INT));
    CHECK(!is_same_family(DataType::BOOLEAN, DataType::VARCHAR));
}

TEST(TypeSystem, CommonTypePromotion) {
    // 整型 -> 更宽的那个
    CHECK(common_type(DataType::TINYINT, DataType::SMALLINT) ==
          DataType::SMALLINT);
    CHECK(common_type(DataType::INT, DataType::TINYINT) == DataType::INT);
    CHECK(common_type(DataType::BIGINT, DataType::INT) == DataType::BIGINT);
    // 布尔参与数值提升
    CHECK(common_type(DataType::BOOLEAN, DataType::SMALLINT) ==
          DataType::SMALLINT);
    // 字符串
    CHECK(common_type(DataType::VARCHAR, DataType::TEXT) == DataType::TEXT);
    CHECK(common_type(DataType::VARCHAR, DataType::VARCHAR) ==
          DataType::VARCHAR);
    // 时间
    CHECK(common_type(DataType::DATE, DataType::DATETIME) == DataType::DATETIME);
    CHECK(common_type(DataType::TIME, DataType::DATE) ==
          DataType::UNKNOWN_TYPE);
    // 跨族
    CHECK(common_type(DataType::INT, DataType::VARCHAR) ==
          DataType::UNKNOWN_TYPE);
}

TEST(TypeSystem, IntegerValuesShareOneStorageAndKey) {
    // 不同逻辑宽度的值在 Value/存储层完全等价
    const Value as_tiny = Value::tinyint(5);
    const Value as_small = Value::smallint(5);
    const Value as_int = Value(5, DataType::INT);
    const Value as_big = Value::bigint(5);

    CHECK(as_tiny == as_small);
    CHECK(as_small == as_int);
    CHECK(as_int == as_big);
    CHECK_EQ(as_tiny.to_key(), as_big.to_key());
    CHECK_EQ(as_tiny.hash(), as_big.hash());

    // Value(5) 规范化成 BIGINT（最宽的整数逻辑类型）
    CHECK(Value(5).type() == DataType::BIGINT);
    CHECK(Value(5).is_int());
}

TEST(TypeSystem, IntegerValuesRetainWidthWhenTyped) {
    // 显式指定宽度时保留逻辑类型（写入时需要做范围校验）
    CHECK(Value::tinyint(5).type() == DataType::TINYINT);
    CHECK(Value::smallint(5).type() == DataType::SMALLINT);
    CHECK(Value(5, DataType::INT).type() == DataType::INT);
    CHECK(Value::bigint(5).type() == DataType::BIGINT);
    // 但它们互相比较/编码一致
    CHECK(sql_equal(Value::tinyint(5), Value::bigint(5)) == Truth::TRUE);
}

TEST(TypeSystem, SchemaRejectsOutOfRangeInteger) {
    TableSchema schema;
    schema.set_name(Identifier("t"));
    schema.add_column(Identifier("id"), DataType::BIGINT, true, false);
    schema.add_column(Identifier("tiny"), DataType::TINYINT, false, true);
    schema.add_column(Identifier("small"), DataType::SMALLINT, false, true);
    schema.add_column(Identifier("normal"), DataType::INT, false, true);

    // 边界值可以写入
    CHECK(row()
              .set(Identifier("id"), 1)
              .set(Identifier("tiny"), Value(127, DataType::TINYINT))
              .set(Identifier("small"), Value(32767, DataType::SMALLINT))
              .set(Identifier("normal"), Value(2147483647LL, DataType::INT))
              .build(schema)
              .has_value());

    // 越界值必须被拒绝
    auto too_big_for_tiny =
        row().set(Identifier("id"), 1).set(Identifier("tiny"), 128).build(schema);
    CHECK(!too_big_for_tiny.has_value());
    CHECK(too_big_for_tiny.error() == SchemaError::VALUE_OUT_OF_RANGE);

    auto too_small = row()
                         .set(Identifier("id"), 1)
                         .set(Identifier("small"), -32769)
                         .build(schema);
    CHECK(!too_small.has_value());
    CHECK(too_small.error() == SchemaError::VALUE_OUT_OF_RANGE);

    auto too_big_for_int = row()
                               .set(Identifier("id"), 1)
                               .set(Identifier("normal"), 2147483648LL)
                               .build(schema);
    CHECK(!too_big_for_int.has_value());
    CHECK(too_big_for_int.error() == SchemaError::VALUE_OUT_OF_RANGE);
}

TEST(TypeSystem, BigintColumnAcceptsAnyIntegerWidth) {
    TableSchema schema;
    schema.set_name(Identifier("t"));
    schema.add_column(Identifier("id"), DataType::BIGINT, true, false);

    // 之前 BIGINT 列 + int 字面量会报 Type mismatch
    CHECK(row().set(Identifier("id"), 5).build(schema).has_value());
    CHECK(row().set(Identifier("id"), Value::tinyint(5)).build(schema).has_value());
    CHECK(row()
              .set(Identifier("id"), static_cast<int64_t>(1) << 40)
              .build(schema)
              .has_value());
}

TEST(TypeSystem, StoredValueOutOfRangeIsRejectedOnRead) {
    TableSchema schema;
    schema.set_name(Identifier("t"));
    schema.add_column(Identifier("tiny"), DataType::TINYINT, true, false);

    // 手工构造一个"合法 key 但超出列范围"的字节流：300 编成 TINYINT 列
    const Key bad_key = Value(300).to_key(DataType::TINYINT);
    std::string data;
    data.push_back(static_cast<char>(Row::kFormatVersion));
    for (int i = 3; i >= 0; --i) {          // field_count = 1 (大端)
        data.push_back(static_cast<char>((1u >> (8 * i)) & 0xFF));
    }
    for (int i = 3; i >= 0; --i) {          // key 长度
        data.push_back(static_cast<char>((bad_key.size() >> (8 * i)) & 0xFF));
    }
    data += bad_key;

    auto restored = Row::deserialize(data, schema);
    CHECK(!restored.has_value());
    CHECK(restored.error() == SchemaError::VALUE_OUT_OF_RANGE);
}

TEST(TypeSystem, TypedKeyEncodingUsesColumnFamilyForNull) {
    const Value null_value;
    CHECK_EQ(null_value.to_key(DataType::INT),
             Value::min_key_for_type(DataType::INT));
    CHECK_EQ(null_value.to_key(DataType::BIGINT),
             Value::min_key_for_type(DataType::BIGINT));
    CHECK_EQ(null_value.to_key(DataType::TEXT),
             Value::min_key_for_type(DataType::TEXT));
    // 所有整型宽度共享同一个 NULL key
    CHECK_EQ(null_value.to_key(DataType::TINYINT),
             null_value.to_key(DataType::SMALLINT));
}
