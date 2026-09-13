# tests/test_session/CMakeLists.txt
cmake_minimum_required(VERSION 3.20)

add_executable(test_session
    main.cpp
    test_pipeline.cpp
    test_errors.cpp
    test_explain.cpp
    test_metadata.cpp
    test_cost.cpp
)

target_link_libraries(test_session PRIVATE
    sql_session
    test_fwk
)

add_test(NAME test_session COMMAND test_session)

# CLI 冒烟测试：跑真实可执行文件（sqldb），只看退出码
add_test(NAME cli_smoke_ok
    COMMAND sqldb --engine=mock ${CMAKE_CURRENT_SOURCE_DIR}/cli_smoke.sql)
add_test(NAME cli_smoke_fail
    COMMAND sqldb --engine=mock ${CMAKE_CURRENT_SOURCE_DIR}/cli_smoke_fail.sql)
set_tests_properties(cli_smoke_fail PROPERTIES WILL_FAIL TRUE)
