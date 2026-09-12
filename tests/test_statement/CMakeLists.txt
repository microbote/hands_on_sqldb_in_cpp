# tests/test_statement/CMakeLists.txt
cmake_minimum_required(VERSION 3.20)

add_executable(test_statement
    main.cpp
    test_builder.cpp
    test_validator.cpp
    test_pipeline.cpp
    test_source_span.cpp
    test_highlight.cpp
)

target_link_libraries(test_statement PRIVATE
    sql_statement
    sql_parser_lib
    sql_types
    test_fwk        # test_framework.h + memory_catalog.h（共享测试辅助）
)

add_test(NAME test_statement COMMAND test_statement)
