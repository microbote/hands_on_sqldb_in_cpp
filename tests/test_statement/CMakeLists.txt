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

target_include_directories(test_statement PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}   # memory_catalog.h
)

target_link_libraries(test_statement PRIVATE
    sql_statement
    sql_parser_lib
    sql_types
    test_fwk
)

add_test(NAME test_statement COMMAND test_statement)
