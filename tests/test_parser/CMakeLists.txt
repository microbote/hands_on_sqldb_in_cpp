# tests/test_parser/CMakeLists.txt
cmake_minimum_required(VERSION 3.20)

add_executable(test_parser
    main.cpp
    test_ast_nodes.cpp
    test_type_parsing.cpp
    test_literals.cpp
    test_clauses.cpp
    test_sql_statements.cpp
    test_error_reporting.cpp
)

# 链接 parser（自带 flex/bison 生成步骤）与 sql_types（用于别名一致性测试）
target_link_libraries(test_parser PRIVATE
    sql_parser_lib
    sql_types
    test_fwk
)

add_test(NAME test_parser COMMAND test_parser)
