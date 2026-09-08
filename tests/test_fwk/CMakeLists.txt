# tests/fwk/CMakeLists.txt
cmake_minimum_required(VERSION 3.20)

# 查找 fmt 库
find_package(fmt REQUIRED)

# 创建测试框架接口库（header-only）
add_library(test_fwk INTERFACE)

# 包含路径
target_include_directories(test_fwk INTERFACE
    ${CMAKE_CURRENT_SOURCE_DIR}
)

# 链接 fmt
target_link_libraries(test_fwk INTERFACE
    fmt::fmt
)

# 不需要编译任何 .cpp 文件（因为 run_all 是 inline）