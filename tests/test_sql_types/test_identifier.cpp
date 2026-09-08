// db_dev/tests/test_sql_types/test_identifier.cpp
#include "test_framework.h"
#include "sql_types/identifier.h"

using namespace sql;

TEST(Identifier, Basic) {
    Identifier id1("users");
    CHECK_EQ(id1.str(), "users");
    CHECK(!id1.empty());
}