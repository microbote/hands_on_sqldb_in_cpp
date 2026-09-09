// test_identifier.cpp
#include "test_framework.h"
#include "sql_types/identifier.h"

using namespace sql;

TEST(Identifier, DefaultConstructor) {
    Identifier id;
    CHECK(id.empty());
}

TEST(Identifier, StringConstructor) {
    Identifier id("users");
    CHECK(!id.empty());
    CHECK_EQ(id.str(), "users");
    
    Identifier id2(std::string("orders"));
    CHECK_EQ(id2.str(), "orders");
}

TEST(Identifier, CaseInsensitiveComparison) {
    // 不加引号时，大小写不敏感
    Identifier a("UserID");
    Identifier b("userid");
    CHECK(a == b);
    CHECK(!(a != b));
    
    // 不同字符串
    Identifier c("username");
    CHECK(a != c);
}

TEST(Identifier, QuotedIdentifier) {
    // 加引号保留大小写
    auto quoted = identifier("\"UserName\"");
    auto normal = identifier("username");
    
    CHECK(quoted == normal);  // 不管带不带引号，统一小写比较 "UserName" == "username"
}

TEST(Identifier, Normalization) {
    // 不加引号时统一小写
    auto Upper = identifier("UserName");
    CHECK_NE(Upper.str(), std::string("username"));
    CHECK_EQ(Upper.lower(), std::string("username"));
    CHECK(Upper == "username");
    
    auto mixed = identifier("MiXeDcAsE");
    CHECK_EQ(mixed.lower(), "mixedcase");
}

TEST(Identifier, DisplayName) {
    CHECK_EQ(identifier("user_name").display_name(), "user_name");
    
    auto quoted = identifier("\"UserName\"");
    // 加引号保持原样或添加引号
    CHECK_EQ(quoted.display_name(), "\"UserName\"");
}

TEST(Identifier, Hash) {
    // 相同内容（忽略大小写）哈希一致
    Identifier a("UserID");
    Identifier b("userid");
    CHECK_EQ(a.hash(), b.hash());
    
    // 可用于 unordered_map/set
    {
    std::unordered_map<Identifier, int, IdentifierHash> map;
    map[a] = 1;
    auto it = map.find(b);
    CHECK(it != map.end());
    CHECK_EQ(it->second, 1);
    }

    {
    std::unordered_map<Identifier, int> map;
    map[b] = 1;
    auto it = map.find(a);
    CHECK(it != map.end());
    CHECK_EQ(it->second, 1);
    }
}

TEST(Identifier, StringInterop) {
    Identifier id("users");
    
    // 转换为 string
    std::string s = id.str();
    CHECK_EQ(s, "users");
    
    // 与 string 比较
    CHECK(id == "users");
    CHECK(id == std::string("USERS"));  // 大小写不敏感
    
    // 拼接
    std::string full = "my_" + id.str();
    CHECK_EQ(full, "my_users");
}

TEST(Identifier, Sorting) {
    std::vector<Identifier> ids = {
        Identifier("b"),
        Identifier("A"),
        Identifier("c")
    };
    
    std::sort(ids.begin(), ids.end());
    // 按字母序（忽略大小写）
    CHECK_EQ(ids[0], "a");
    CHECK_EQ(ids[1], "b");
    CHECK_EQ(ids[2], "c");
}