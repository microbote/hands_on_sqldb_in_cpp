# tests/test_relation/CMakeLists.txt
cmake_minimum_required(VERSION 3.20)

add_executable(test_relation
    main.cpp
    test_key_prefix.cpp
    test_table.cpp
    test_catalog.cpp
)

target_link_libraries(test_relation PRIVATE
    sql_relation   # 自动带上 sql_types + sql_kv
    test_fwk
)

add_test(NAME test_relation COMMAND test_relation)
