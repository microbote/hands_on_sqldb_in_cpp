# tests/test_server/CMakeLists.txt
#
# 服务器：配置、协议、ServiceThread/Loop、端到端（mock 引擎 + 真实 socket）。
cmake_minimum_required(VERSION 3.20)

find_package(Threads REQUIRED)

add_executable(test_server
    main.cpp
    test_config.cpp
    test_logger.cpp
    test_protocol.cpp
    test_e2e.cpp
    test_client.cpp
    test_lifecycle.cpp
    test_remote_client_fake_server.cpp
)

target_link_libraries(test_server PRIVATE
    sql_server
    sql_client   # client::RemoteConnection + 共用 REPL
    test_fwk
    Threads::Threads
)

# 源码树路径：用来直接读 server/etc/sqldb-server.conf，跟内置默认值逐键比对
# （示例配置飘了要在测试里变红，而不是等人照着改出问题）
target_compile_definitions(test_server PRIVATE
    SQLDB_SOURCE_DIR="${CMAKE_SOURCE_DIR}")

add_test(NAME test_server COMMAND test_server)
