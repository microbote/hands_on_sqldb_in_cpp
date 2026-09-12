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
# 查找 flex / bison（parser 模块）
#   bison 必须是 3.x：sql.y 用了 %code / %define parse.error
#   本机（macports/homebrew）的 bison 3.8 在：
#     /usr/local/opt/bison/bin/bison
#   系统自带的 /usr/bin/bison 是 2.3，不能用。
# ============================================================
if(APPLE)
    set(BISON_EXECUTABLE "/usr/local/opt/bison/bin/bison" CACHE FILEPATH "Path to GNU Bison 3.x")
endif()
find_package(BISON 3.0 REQUIRED)
find_package(FLEX REQUIRED)

# ============================================================
# parser 静态库（C 风格 AST + Flex/Bison；不依赖 sql_types）
#   生成物统一放到 build/parser/，源码目录里不再保留生成文件
# ============================================================
set(PARSER_GEN_DIR ${CMAKE_CURRENT_BINARY_DIR}/parser)
file(MAKE_DIRECTORY ${PARSER_GEN_DIR})

bison_target(sql_parser
    ${CMAKE_CURRENT_SOURCE_DIR}/parser/sql.y
    ${PARSER_GEN_DIR}/parser.tab.c
    DEFINES_FILE ${PARSER_GEN_DIR}/parser.tab.h
)
flex_target(sql_lexer
    ${CMAKE_CURRENT_SOURCE_DIR}/parser/sql.l
    ${PARSER_GEN_DIR}/lex.yy.c
    DEFINES_FILE ${PARSER_GEN_DIR}/lex.yy.h
)
add_flex_bison_dependency(sql_lexer sql_parser)

add_library(sql_parser_lib STATIC
    parser/ast.cpp
    parser/parser.cpp
    ${BISON_sql_parser_OUTPUTS}
    ${FLEX_sql_lexer_OUTPUTS}
)

target_include_directories(sql_parser_lib PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}         # "common/c_types.h"
    ${CMAKE_CURRENT_SOURCE_DIR}/parser  # "ast.h" / "parser.h"
    ${PARSER_GEN_DIR}                   # 生成的 parser.tab.h / lex.yy.h
)

# 生成代码按 C 编译（parser 保持 C 风格；ast.h 用 extern "C" 供 C++ 侧复用）。
# -w：关掉 flex/bison 生成代码自身的告警（不是我们的代码）
set_source_files_properties(
    ${PARSER_GEN_DIR}/parser.tab.c
    ${PARSER_GEN_DIR}/lex.yy.c
    PROPERTIES COMPILE_FLAGS "-w"
)

target_compile_features(sql_parser_lib PUBLIC cxx_std_23)

# ============================================================
# sql_types 静态库（同时提供头文件路径）
# ============================================================
add_library(sql_types STATIC
    sql_types/compare_op.cpp
    sql_types/key_range.cpp
    sql_types/row.cpp
    sql_types/schema.cpp
    sql_types/temporal.cpp
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

# ============================================================
# statement 静态库：AST -> sql::Query（StatementBuilder）
#                     语义校验（StatementValidator）
# ============================================================
add_library(sql_statement STATIC
    statement/source_span.cpp
    statement/sql_highlight.cpp
    statement/stmt_builder.cpp
    statement/stmt_validator.cpp
)

target_include_directories(sql_statement PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}            # "sql_types/query.h" ...
    ${CMAKE_CURRENT_SOURCE_DIR}/sql_types  # "catalog.h" / "schema.h"
    ${CMAKE_CURRENT_SOURCE_DIR}/statement  # "stmt_defs.h"
)

target_link_libraries(sql_statement PUBLIC
    sql_types
    sql_parser_lib
)

target_compile_features(sql_statement PUBLIC cxx_std_23)

# ============================================================
# planner 静态库：Query -> 重写 -> 优化（OptimizedQuery）-> Plan 树
# ============================================================
add_library(sql_planner STATIC
    planner/plan.cpp
    planner/planner.cpp
    planner/optimizer.cpp
    planner/rewriter.cpp
)

target_include_directories(sql_planner PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}            # "planner/optimizer.h" / "sql_types/query.h"
    ${CMAKE_CURRENT_SOURCE_DIR}/sql_types  # "catalog.h" / "schema.h"
    ${CMAKE_CURRENT_SOURCE_DIR}/planner    # "planner_defs.h" / "rewriter.h"
)

target_link_libraries(sql_planner PUBLIC
    sql_types
)

target_compile_features(sql_planner PUBLIC cxx_std_23)

# ============================================================
# KV 存储层（mock 引擎；leveldb 仍走 legacy Makefile，
# 需要时把 leveldb_engine.cpp + ../Debug 的头/库接进来即可）
# ============================================================
add_library(sql_kv STATIC
    storage/mock_engine/mock_engine.cpp
)

target_include_directories(sql_kv PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
)

target_compile_features(sql_kv PUBLIC cxx_std_23)

# ============================================================
# relation 关系层：sql::Catalog 的 KV 实现 + Table/Cursor 视图
# ============================================================
add_library(sql_relation STATIC
    relation/cursor.cpp
    relation/kv_catalog.cpp
    relation/table.cpp
)

target_include_directories(sql_relation PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}            # "storage/kv_engine/kv_engine.h"
    ${CMAKE_CURRENT_SOURCE_DIR}/relation   # "key_prefix.h" / "table.h"
    ${CMAKE_CURRENT_SOURCE_DIR}/sql_types  # "catalog.h" / "schema.h"
)

target_link_libraries(sql_relation PUBLIC
    sql_types
    sql_kv
)

target_compile_features(sql_relation PUBLIC cxx_std_23)

# ============================================================
# executor 执行层：计划树 -> Volcano 算子树 -> 客户端游标
# ============================================================
add_library(sql_executor STATIC
    executor/executor.cpp
)

target_include_directories(sql_executor PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/executor
    ${CMAKE_CURRENT_SOURCE_DIR}/planner
    ${CMAKE_CURRENT_SOURCE_DIR}/relation
    ${CMAKE_CURRENT_SOURCE_DIR}/sql_types
)

target_link_libraries(sql_executor PUBLIC
    sql_planner
    sql_relation
)

target_compile_features(sql_executor PUBLIC cxx_std_23)

# 添加测试子目录
enable_testing()
add_subdirectory(tests)
