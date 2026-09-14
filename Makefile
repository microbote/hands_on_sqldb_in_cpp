# ============================================================
# 构建入口：**只有 CMake 一套构建系统**
#
# 这个文件现在只是一层薄包装（`make xxx` -> `cmake`/`ctest`），方便习惯
# 敲 make 的人。以前那套手写的 Makefile（自己写 g++ 规则、编 parser/relation/
# query 的源文件、链接 tests/test_*.cpp 那几个手工 demo）引用的源文件早已
# 被删掉/搬迁（query/、relation/value.cpp、tests/test_relation.cpp ...），
# 已经不可能编过，所以整段移除 —— 需要历史就翻 git。
#
# 测试分布在 tests/test_<模块>/，每个模块一个可执行文件 + 一个 ctest 用例；
# 凡是不涉及引擎特性的用例都在 Mock 与 LevelDB 上各跑一遍（语义必须一致）。
# ============================================================

BUILD_DIR = build
JOBS      ?= 4

.PHONY: all configure build sqldb cli client server test storage-test svrkit-test server-test tx-test session-test \
        executor-test relation-test planner-test statement-test parser-test \
        sql-types-test cmake-test clean distclean help

all: build

configure:
	cmake -S . -B $(BUILD_DIR)

build: configure
	cmake --build $(BUILD_DIR) -j$(JOBS)

sqldb: configure
	cmake --build $(BUILD_DIR) -j$(JOBS) --target sqldb
	@echo "✅ $(BUILD_DIR)/sqldb 可用（./$(BUILD_DIR)/sqldb --help）"

# 兼容老命令
cli: sqldb

# 服务器：build/sqldb-server --config=<file>（见 server/ 与 tests/test_server/）
server: configure
	cmake --build $(BUILD_DIR) -j$(JOBS) --target sqldb-server
	@echo "✅ $(BUILD_DIR)/sqldb-server 可用（--config=<file>）"

# 通用协程服务器框架（common/net + common/svrkit）
svrkit-test: configure
	cmake --build $(BUILD_DIR) -j$(JOBS)
	./$(BUILD_DIR)/run_tests/test_svrkit

server-test: configure
	cmake --build $(BUILD_DIR) -j$(JOBS)
	./$(BUILD_DIR)/run_tests/test_server

# 远程客户端（连 sqldb-server）
client: configure
	cmake --build $(BUILD_DIR) -j$(JOBS) --target sqldb-client
	@echo "✅ $(BUILD_DIR)/sqldb-client 可用（--host=... --port=...）"

# ---- 单个模块的测试 ----
sql-types-test: configure
	cmake --build $(BUILD_DIR) -j$(JOBS)
	./$(BUILD_DIR)/run_tests/test_sql_types

parser-test: configure
	cmake --build $(BUILD_DIR) -j$(JOBS)
	./$(BUILD_DIR)/run_tests/test_parser

statement-test: configure
	cmake --build $(BUILD_DIR) -j$(JOBS)
	./$(BUILD_DIR)/run_tests/test_statement

planner-test: configure
	cmake --build $(BUILD_DIR) -j$(JOBS)
	./$(BUILD_DIR)/run_tests/test_planner

relation-test: configure
	cmake --build $(BUILD_DIR) -j$(JOBS)
	./$(BUILD_DIR)/run_tests/test_relation

executor-test: configure
	cmake --build $(BUILD_DIR) -j$(JOBS)
	./$(BUILD_DIR)/run_tests/test_executor

session-test: configure
	cmake --build $(BUILD_DIR) -j$(JOBS)
	./$(BUILD_DIR)/run_tests/test_session

tx-test: configure
	cmake --build $(BUILD_DIR) -j$(JOBS)
	./$(BUILD_DIR)/run_tests/test_tx

storage-test: configure
	cmake --build $(BUILD_DIR) -j$(JOBS)
	./$(BUILD_DIR)/run_tests/test_storage

# ---- 全部测试（ctest，含两个 CLI 冒烟测试）----
test: cmake-test

cmake-test: configure
	cmake --build $(BUILD_DIR) -j$(JOBS)
	ctest --test-dir $(BUILD_DIR) --output-on-failure

clean:
	rm -rf $(BUILD_DIR)
	@echo "✅ 清理完成"

distclean: clean
	rm -rf sql_db *.dSYM
	@echo "✅ 完全清理完成"

help:
	@echo "构建/测试都走 CMake；常用目标："
	@echo "  make build            - 构建全部（库 + 测试 + sqldb）"
	@echo "  make sqldb            - 只构建 CLI"
	@echo "  make test             - 跑 ctest 全部用例（含 CLI 冒烟）"
	@echo "  make svrkit-test      - 通用网络/协程服务器框架"
	@echo "  make storage-test     - 存储层（Mock + LevelDB）"
	@echo "  make tx-test          - 事务/多连接（Mock + LevelDB）"
	@echo "  make session-test / planner-test / executor-test / ..."
	@echo "  make clean            - 清掉 build/"
