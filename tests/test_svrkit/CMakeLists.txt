# tests/test_svrkit/CMakeLists.txt
#
# common/svrkit + common/net 的独立测试：不依赖 SQL 协议或存储。
cmake_minimum_required(VERSION 3.20)

find_package(Threads REQUIRED)

add_executable(test_svrkit
    main.cpp
    test_poller.cpp
    test_service.cpp
    test_tcp_server.cpp
)

target_link_libraries(test_svrkit PRIVATE
    svrkit
    test_fwk
    Threads::Threads
)

add_test(NAME test_svrkit COMMAND test_svrkit)
