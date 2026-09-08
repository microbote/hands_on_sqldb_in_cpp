# db_dev/tests/CMakeLists.txt
cmake_minimum_required(VERSION 3.16)

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/run_tests)

# 添加测试框架
add_subdirectory(test_fwk)

# 添加各个模块的测试
add_subdirectory(test_sql_types)

# 未来其他模块的测试
# add_subdirectory(test_parser)
# add_subdirectory(test_storage)