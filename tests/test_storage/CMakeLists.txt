# tests/test_storage/CMakeLists.txt
#
# 存储层测试：KVStore/KVEngine 的接口语义（Mock 与 LevelDB 必须一致）、
# 扫描族、生命周期与持久化。用例来源：旧的 tests/test_mock_engine.cpp /
# test_leveldb_engine.cpp（那两个是手工 main + cout 的 demo，已删除）。
cmake_minimum_required(VERSION 3.20)

add_executable(test_storage
    main.cpp
    test_kv_basics.cpp
    test_scan.cpp
    test_lifecycle.cpp
)

target_link_libraries(test_storage PRIVATE
    sql_kv
    test_fwk
)

add_test(NAME test_storage COMMAND test_storage)
