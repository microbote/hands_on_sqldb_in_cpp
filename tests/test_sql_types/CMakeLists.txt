# tests/test_sql_types/CMakeLists.txt
cmake_minimum_required(VERSION 3.20)

# 查找 fmt 库（已经由 fwk 传递链接，但确保）
find_package(fmt REQUIRED)

# sql_types 头文件路径
set(SQL_TYPES_INCLUDE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../../sql_types)

# 测试可执行文件
add_executable(test_sql_types
    main.cpp
    test_identifier.cpp
)


# 包含路径
target_include_directories(test_sql_types PRIVATE
    ${SQL_TYPES_INCLUDE_DIR}          # sql_types 头文件
)

# 链接 sql_types，自动获得：
#  - sql_types/ 头文件路径（PUBLIC include）
#  - fmt（PUBLIC link）
# 链接库
target_link_libraries(test_sql_types PRIVATE
    sql_types
    test_fwk                          # 测试框架（包含 fmt）
)


# 注册测试
add_test(NAME test_sql_types COMMAND test_sql_types)


