# tests/test_planner/CMakeLists.txt
cmake_minimum_required(VERSION 3.20)

add_executable(test_planner
    main.cpp
    test_rewriter.cpp
    test_optimizer.cpp
    test_planner.cpp
)

target_link_libraries(test_planner PRIVATE
    sql_planner
    sql_statement   # parser + StatementBuilder，用来造真实 Query
    test_fwk        # test_framework.h + memory_catalog.h
)

add_test(NAME test_planner COMMAND test_planner)
