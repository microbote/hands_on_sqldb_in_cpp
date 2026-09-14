# Raft core 的独立测试：不碰 socket、不依赖 SQL 协议。
cmake_minimum_required(VERSION 3.20)

add_executable(test_raft
    main.cpp
    test_raft.cpp
)

target_include_directories(test_raft PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/../.. 
)

target_link_libraries(test_raft PRIVATE
    sql_raft
    test_fwk
)

target_compile_features(test_raft PRIVATE cxx_std_23)

add_test(NAME test_raft COMMAND test_raft)
