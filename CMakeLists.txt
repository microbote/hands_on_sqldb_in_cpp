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
# KV 存储层：mock 引擎必带；leveldb 若在 ../Debug 里能找到头/库就一起编
# （CLI 的 --engine=leveldb 依赖它，找不到时只剩 mock）
# ============================================================
add_library(sql_kv STATIC
    storage/mock_engine/mock_engine.cpp
    storage/kv_engine/tx_buffer.cpp
    storage/kv_engine/merging_iterator.cpp
)

target_include_directories(sql_kv PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
)

target_compile_features(sql_kv PUBLIC cxx_std_23)

find_path(LEVELDB_INCLUDE_DIR NAMES leveldb/db.h
    PATHS ${CMAKE_CURRENT_SOURCE_DIR}/../Debug/include)
find_library(LEVELDB_LIBRARY NAMES leveldb
    PATHS ${CMAKE_CURRENT_SOURCE_DIR}/../Debug/lib)

if(LEVELDB_INCLUDE_DIR AND LEVELDB_LIBRARY)
    message(STATUS "leveldb found: ${LEVELDB_LIBRARY}")
    target_sources(sql_kv PRIVATE
        storage/kv_engine/kv_factory.cpp
        storage/leveldb_engine/leveldb_engine.cpp
    )
    target_include_directories(sql_kv PUBLIC ${LEVELDB_INCLUDE_DIR})
    target_link_libraries(sql_kv PUBLIC ${LEVELDB_LIBRARY})
    target_compile_definitions(sql_kv PUBLIC SQLDB_HAVE_LEVELDB=1)
else()
    message(STATUS "leveldb not found: CLI default engine will be mock")
endif()

# ============================================================
# Raft core：P0 是线程无关、socket 无关的确定性实现。
# 它不接入 server 入口，也不认识 SQL；后续 P1 再增加 KV 适配层。
# ============================================================
add_library(sql_raft STATIC
    raft/group_router.cpp
    raft/kv_state_machine.cpp
    raft/message_codec.cpp
    raft/peers.cpp
    raft/raft_kv_store.cpp
    raft/raft_runtime.cpp
    raft/tcp_transport.cpp
    raft/proposal_payload.cpp
    raft/raft_node.cpp
)

target_include_directories(sql_raft PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
)

target_link_libraries(sql_raft PUBLIC
    sql_kv
    svrkit
)

if(LEVELDB_INCLUDE_DIR AND LEVELDB_LIBRARY)
    target_sources(sql_raft PRIVATE
        raft/leveldb_request_result_store.cpp
        raft/leveldb_log_store.cpp
    )
    target_include_directories(sql_raft PUBLIC ${LEVELDB_INCLUDE_DIR})
    target_link_libraries(sql_raft PUBLIC ${LEVELDB_LIBRARY})
    target_compile_definitions(sql_raft PUBLIC SQLDB_HAVE_LEVELDB=1)
endif()

target_compile_features(sql_raft PUBLIC cxx_std_23)

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

# ============================================================
# session 会话层：一条 SQL -> 游标（串起 parser/statement/planner/executor）
# ============================================================
add_library(sql_session STATIC
    session/session.cpp
)

target_include_directories(sql_session PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/session
)

target_link_libraries(sql_session PUBLIC
    sql_executor    # 自动带上 sql_planner + sql_relation + sql_types + sql_kv
    sql_statement   # parser + StatementBuilder + StatementValidator + highlight
)

target_compile_features(sql_session PUBLIC cxx_std_23)

# ============================================================
# 命令行客户端 sqldb：SQL 文本 -> session -> 表格输出
# ============================================================
# ============================================================
# common/net：跨平台网络原语
#   - Poller：kqueue / epoll / poll 统一成 poll 词汇、水平触发
#   - TcpSocket：阻塞式客户端与 svrkit 共用的连接/监听/所有权封装
# ============================================================
add_library(common_net STATIC
    common/net/poller_poll.cpp
    common/net/poller_kqueue.cpp
    common/net/poller_epoll.cpp
    common/net/socket.cpp
)

target_include_directories(common_net PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
)

target_compile_features(common_net PUBLIC cxx_std_23)

# ============================================================
# common/svrkit：协议无关的协程服务器框架（后续 Raft 复用）
#   Task / Loop / ServiceThread / TcpServer
# ============================================================
add_library(svrkit STATIC
    common/svrkit/loop.cpp
    common/svrkit/service.cpp
    common/svrkit/tcp_server.cpp
)

target_include_directories(svrkit PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
)

find_package(Threads REQUIRED)
target_link_libraries(svrkit PUBLIC
    common_net
    Threads::Threads
)

target_compile_features(svrkit PUBLIC cxx_std_23)

# ============================================================
# common/proto：sqldb 的线协议（帧/元信息编解码）
#
#   放在 common/ 下是因为**两端都用**：server 收帧/回帧、client 发帧/解帧。
#   和 common/net、common/svrkit 一样，属于"协议无关/可复用"的那一层。
# ============================================================
add_library(common_proto STATIC
    common/proto/protocol.cpp
)

target_include_directories(common_proto PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
)

target_link_libraries(common_proto PUBLIC
    sql_types
    fmt::fmt
)

target_compile_features(common_proto PUBLIC cxx_std_23)

# ============================================================
# sqldb 服务器应用层：server/ —— SQL 协议 + Session 路由
#   TCP/协程生命周期在 svrkit；本地 CLI（sqldb）不依赖它。
# ============================================================
add_library(sql_server STATIC
    server/config.cpp
    server/logger.cpp
    server/raft_bootstrap.cpp
    server/server.cpp
)

target_include_directories(sql_server PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/server
)

target_link_libraries(sql_server PUBLIC
    sql_session
    sql_kv
    sql_raft
    common_proto
    svrkit
    fmt::fmt
)

target_compile_features(sql_server PUBLIC cxx_std_23)

add_executable(sqldb-server server/main_server.cpp)
target_link_libraries(sqldb-server PRIVATE sql_server)

# 服务器以"可发布目录"的形状产出：build/svr/{bin,etc} ——
#   svr/bin/sqldb-server           可执行
#   svr/etc/sqldb-server.conf      带注释的默认配置（直接从源码树复制）
# 客户端的 sqldb / sqldb-client 仍旧留在 build/ 根下（它们不是服务端件）。
set_target_properties(sqldb-server PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/svr/bin)

# 每次构建都把默认配置同步过去（改源文件后重建即生效；COPYONLY 不做变量替换）
add_custom_command(TARGET sqldb-server POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_BINARY_DIR}/svr/etc
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            ${CMAKE_CURRENT_SOURCE_DIR}/server/etc/sqldb-server.conf
            ${CMAKE_BINARY_DIR}/svr/etc/sqldb-server.conf
    COMMENT "复制默认配置到 svr/etc/sqldb-server.conf")

# ============================================================
# 客户端层：client/ —— REPL 与 transport 解耦
#
#   client/           共享库（无 main）：connection（本地/远程两个 transport）
#                     + repl（共用交互层）
#   client/local/     本地客户端 main -> build/sqldb
#   client/remote/    远程客户端 main -> build/sqldb-client
# ============================================================
add_library(sql_client STATIC
    client/connection.cpp
    client/repl.cpp
)

target_include_directories(sql_client PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/client
)

# readline：**行编辑/历史/补全**都靠它。
#
# 注意（踩过的坑）：find_path/find_library 的默认搜索顺序会把 macOS SDK 里的
# `libreadline.tbd`（其实是 libedit 的兼容层）排在前面的 PATHS 之前，
# 于是编出来的二进制链接的是 /usr/lib/libedit.dylib，历史/补全行为不对。
# 所以先按 macports/homebrew 的**明确路径**找（NO_DEFAULT_PATH），
# 找不到再退回系统默认搜索（比如 Linux 上的 libreadline-dev）。
find_path(READLINE_INCLUDE_DIR
    NAMES readline/readline.h readline/history.h
    PATHS /opt/local/include
          /usr/local/opt/readline/include
          /opt/homebrew/opt/readline/include
    NO_DEFAULT_PATH)
find_library(READLINE_LIBRARY
    NAMES readline
    PATHS /opt/local/lib
          /usr/local/opt/readline/lib
          /opt/homebrew/opt/readline/lib
    NO_DEFAULT_PATH)
if(NOT READLINE_INCLUDE_DIR OR NOT READLINE_LIBRARY)
    # 退回系统路径（Linux 常见：/usr/include + /usr/lib/libreadline.so）
    find_path(READLINE_INCLUDE_DIR NAMES readline/readline.h readline/history.h)
    find_library(READLINE_LIBRARY NAMES readline)
endif()

target_link_libraries(sql_client PUBLIC
    sql_session
    common_proto   # 复用协议编解码，不链接服务器框架
    common_net            # 阻塞式 TCP transport
    fmt::fmt
)

target_compile_features(sql_client PUBLIC cxx_std_23)

if(READLINE_INCLUDE_DIR AND READLINE_LIBRARY)
    message(STATUS "readline: ${READLINE_LIBRARY} (include: ${READLINE_INCLUDE_DIR})")
    # 头文件与宏只给 sql_client 自己（只有 repl.cpp 用 readline）：
    # 把 /opt/local/include 传成 PUBLIC 会让别的 TU 也吃到 macports 的头
    # （libc++ 可见性不一致 -> fmt 的 weak symbol 链接告警），所以这里 PRIVATE。
    target_include_directories(sql_client PRIVATE ${READLINE_INCLUDE_DIR})
    target_compile_definitions(sql_client PRIVATE SQLDB_HAVE_READLINE=1)
    # 链接必须是 PUBLIC：最终可执行文件（sqldb / sqldb-client）要能 resolve 符号
    target_link_libraries(sql_client PUBLIC ${READLINE_LIBRARY})
else()
    message(STATUS "readline 未找到：交互模式退回 std::getline（无历史/补全）")
endif()

add_executable(sqldb client/local/main.cpp)

target_link_libraries(sqldb PRIVATE sql_client)

add_executable(sqldb-client client/remote/main.cpp)
target_link_libraries(sqldb-client PRIVATE sql_client)

# 添加测试子目录
enable_testing()
add_subdirectory(tests)
