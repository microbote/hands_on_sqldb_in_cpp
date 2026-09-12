# tests/test_executor/CMakeLists.txt
cmake_minimum_required(VERSION 3.20)

add_executable(test_executor
    main.cpp
    test_select.cpp
    test_sort_limit.cpp
    test_write.cpp
)

target_link_libraries(test_executor PRIVATE
    sql_executor   # 自动带上 sql_planner + sql_relation + sql_types + sql_kv
    sql_statement  # parser + StatementBuilder（造真实 Query）
    test_fwk
)

add_test(NAME test_executor COMMAND test_executor)
