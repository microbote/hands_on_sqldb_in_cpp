# tests/test_tx/CMakeLists.txt
cmake_minimum_required(VERSION 3.20)

add_executable(test_tx
    main.cpp
    test_transaction.cpp
)

target_link_libraries(test_tx PRIVATE
    sql_kv      # 引擎 + 事务缓冲/迭代器
    test_fwk
)

add_test(NAME test_tx COMMAND test_tx)
