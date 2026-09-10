# tests/test_sql_types/CMakeLists.txt
cmake_minimum_required(VERSION 3.20)

# 测试可执行文件
add_executable(test_sql_types
    main.cpp
    test_compare_op.cpp
    test_value.cpp
    test_identifier.cpp
    test_field_type.cpp
    test_row.cpp
    test_condition.cpp
    test_key_set.cpp
    test_key_range.cpp
    test_key_codec.cpp
    test_schema.cpp
    test_serialize.cpp
    test_catalog.cpp
    test_query.cpp
)

# 链接 sql_types，自动获得：
#  - sql_types/ 头文件路径（PUBLIC include）
#  - fmt（PUBLIC link）
target_link_libraries(test_sql_types PRIVATE
    sql_types
    test_fwk                          # 测试框架（包含 fmt）
)

# 注册测试
add_test(NAME test_sql_types COMMAND test_sql_types)

