// tests/test_relation/test_key_prefix.cpp
//
// key 布局契约：名字长度前缀 + 主键保序编码 + 前缀上界。
#include "test_framework.h"

#include <string>

#include "relation/key_prefix.h"
#include "sql_types/value.h"

namespace {

const sql::Identifier kDb("shop");
const sql::Identifier kTable("users");

std::string data_key(int64_t pk) {
  return sql::keys::data_key(kDb, kTable, sql::Value(pk, sql::DataType::INT),
                             sql::DataType::INT);
}

} // namespace

TEST(KeyPrefix, ComponentUsesLengthPrefix) {
  CHECK_EQ(sql::keys::encode_component("users"), std::string("5:users"));
  CHECK_EQ(sql::keys::encode_component(""), std::string("0:"));
  // 名字统一小写：标识符大小写不敏感
  CHECK_EQ(sql::keys::encode_identifier(sql::Identifier("Users")),
           std::string("5:users"));
}

TEST(KeyPrefix, DataKeyIsOrderPreserving) {
  // 这一条正是不能用 Value::to_string() 的原因：
  // 字符串序下 "10" < "2"，而数值序要求 2 < 10
  CHECK(data_key(2) < data_key(10));
  CHECK(data_key(-5) < data_key(0));
  CHECK(data_key(0) < data_key(1));
  CHECK(sql::keys::data_key(kDb, kTable, sql::Value(),
                            sql::DataType::INT) < data_key(0)); // NULL 最小
}

TEST(KeyPrefix, DataKeyCarriesTablePrefix) {
  const std::string prefix = sql::keys::data_prefix(kDb, kTable);
  CHECK_EQ(prefix, std::string("@data/4:shop/5:users/"));
  const std::string key = data_key(7);
  CHECK(key.rfind(prefix, 0) == 0); // 以表前缀开头
  CHECK(sql::keys::is_data_key(key));
  CHECK(!sql::keys::is_system_key(key));
}

TEST(KeyPrefix, PrefixEndBoundsEveryKeyOfTheTable) {
  const std::string prefix = sql::keys::data_prefix(kDb, kTable);
  const std::string end = sql::keys::prefix_end(prefix);
  CHECK_EQ(end, std::string("@data/4:shop/5:users0"));

  for (int64_t pk : {int64_t{0}, int64_t{1}, int64_t{-9}, int64_t{9999}}) {
    const std::string key = data_key(pk);
    CHECK(key >= prefix);
    CHECK(key < end);
  }

  // 另一张表（表名长度不同）落在区间之外
  const std::string other = sql::keys::data_key(
      sql::Identifier("shop"), sql::Identifier("users2"),
      sql::Value(int64_t{1}, sql::DataType::INT), sql::DataType::INT);
  CHECK(!(other >= prefix && other < end));
}

TEST(KeyPrefix, StringPrimaryKeyIsOrderedByBytes) {
  const auto key = [](const char *name) {
    return sql::keys::data_key(kDb, kTable, sql::Value(name),
                               sql::DataType::VARCHAR);
  };
  CHECK(key("alice") < key("bob"));
  CHECK(key("a") < key("ab"));
  // 空串排在任何非空串之前
  CHECK(key("") < key("a"));
}

TEST(KeyPrefix, SystemKeysAreDistinguishable) {
  CHECK(sql::keys::is_system_key(sql::keys::databases()));
  CHECK(sql::keys::is_system_key(sql::keys::db_tables(kDb)));
  CHECK(sql::keys::is_system_key(sql::keys::db_schema(kDb, kTable)));
  CHECK(!sql::keys::is_data_key(sql::keys::db_schema(kDb, kTable)));
}
