# db_dev/CMakeLists.txt
cmake_minimum_required(VERSION 3.16)
project(sqldb_dev LANGUAGES C CXX)

# for clangd
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
# ============================================================
# 查找 fmt
# ============================================================
if(NOT DEFINED fmt_DIR)
    set(fmt_DIR "/usr/local/lib")
endif()
find_package(fmt REQUIRED)

# ============================================================
# sql_types 静态库（同时提供头文件路径）
# ============================================================
add_library(sql_types STATIC
    sql_types/compare_op.cpp
    sql_types/key_range.cpp
    sql_types/row.cpp
    sql_types/schema.cpp
    sql_types/value.cpp
)

# 关键：添加根目录路径，这样 "common/c_types.h" 能被找到
# PUBLIC 头文件路径 - 链接者自动获得 include 路径
target_include_directories(sql_types PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR} 
    ${CMAKE_CURRENT_SOURCE_DIR}/sql_types
)

# 如果 sql_types 中的 cpp 使用了 fmt
target_link_libraries(sql_types PUBLIC
    fmt::fmt
)

# 如果使用 C++23 和 libc++
target_compile_features(sql_types PUBLIC cxx_std_23)

# 添加测试子目录
add_subdirectory(tests)