# ============================================================
# 编译器和编译选项
# ============================================================
CXX      = /usr/local/opt/llvm/bin/clang++
CXXFLAGS = -std=c++20 -stdlib=libc++ -g -Wall -Wextra \
           -I./src -I./src/parser -I./src/storage -I./src/core -I./src/common

LDFLAGS  = -L/usr/local/lib -lleveldb \
           -L/usr/local/opt/llvm/lib/c++ -lc++abi -lc++ \
           -lpthread

FLEX     = flex
YACC     = /usr/local/opt/bison/bin/bison -t

# ============================================================
# 目录路径
# ============================================================
SRC_DIR     = src
PARSER_DIR  = $(SRC_DIR)/parser
STORAGE_DIR = $(SRC_DIR)/storage
CORE_DIR    = $(SRC_DIR)/core
COMMON_DIR  = $(SRC_DIR)/common
TESTS_DIR   = $(SRC_DIR)/tests
BUILD_DIR   = build

# ============================================================
# Parser 文件 (Flex/Bison)
# ============================================================
LEX_SRC  = $(PARSER_DIR)/sql.l
YACC_SRC = $(PARSER_DIR)/sql.y
LEX_OUT  = $(PARSER_DIR)/lex.yy.c
LEX_HDR  = $(PARSER_DIR)/lex.yy.h
YACC_OUT = $(PARSER_DIR)/parser.tab.c
YACC_HDR = $(PARSER_DIR)/parser.tab.h

# ============================================================
# Parser 源文件
# ============================================================
AST_SRC  = $(PARSER_DIR)/ast.cpp
AST_HDR  = $(PARSER_DIR)/ast.h
AST_OBJ  = $(BUILD_DIR)/ast.o

# ============================================================
# Storage 源文件
# ============================================================
STORAGE_HDR = $(STORAGE_DIR)/storage_engine.h
MOCK_SRC    = $(STORAGE_DIR)/mock_engine.cpp
MOCK_HDR    = $(STORAGE_DIR)/mock_engine.h
MOCK_OBJ    = $(BUILD_DIR)/mock_engine.o

LEVELDB_SRC = $(STORAGE_DIR)/leveldb_engine.cpp
LEVELDB_HDR = $(STORAGE_DIR)/leveldb_engine.h
LEVELDB_OBJ = $(BUILD_DIR)/leveldb_engine.o

# ============================================================
# Core 源文件
# ============================================================
CONDITION_SRC = $(CORE_DIR)/condition.cpp
CONDITION_HDR = $(CORE_DIR)/condition.h
CONDITION_OBJ = $(BUILD_DIR)/condition.o

STATEMENT_SRC = $(CORE_DIR)/statement_builder.cpp
STATEMENT_HDR = $(CORE_DIR)/statement.h $(CORE_DIR)/statement_builder.h
STATEMENT_OBJ = $(BUILD_DIR)/statement_builder.o

OPTIMIZER_SRC = $(CORE_DIR)/optimizer.cpp
OPTIMIZER_HDR = $(CORE_DIR)/optimizer.h $(CORE_DIR)/plan.h
OPTIMIZER_OBJ = $(BUILD_DIR)/optimizer.o

EXECUTOR_SRC  = $(CORE_DIR)/executor.cpp
EXECUTOR_HDR  = $(CORE_DIR)/executor.h
EXECUTOR_OBJ  = $(BUILD_DIR)/executor.o

# ============================================================
# Common 源文件
# ============================================================
COMMON_HDR = $(COMMON_DIR)/row_builder.h

# ============================================================
# Flex/Bison 对象
# ============================================================
LEX_OBJ  = $(BUILD_DIR)/lex.yy.o
YACC_OBJ = $(BUILD_DIR)/parser.tab.o

# ============================================================
# 主程序
# ============================================================
MAIN_SRC = $(SRC_DIR)/main.cpp
MAIN_OBJ = $(BUILD_DIR)/main.o
TARGET   = sql_engine

# ============================================================
# Core 对象（所有测试共享）
# ============================================================
CORE_OBJS = $(AST_OBJ) $(CONDITION_OBJ) $(STATEMENT_OBJ) \
            $(OPTIMIZER_OBJ) $(EXECUTOR_OBJ) $(MOCK_OBJ) \
            $(LEVELDB_OBJ) $(LEX_OBJ) $(YACC_OBJ)

# ============================================================
# 测试程序
# ============================================================
TEST_PARSER_SRC    = $(TESTS_DIR)/test_parser.cpp
TEST_PARSER_OBJ    = $(BUILD_DIR)/test_parser.o
TEST_PARSER_TARGET = test_parser

TEST_STATEMENT_SRC = $(TESTS_DIR)/test_statement.cpp
TEST_STATEMENT_OBJ = $(BUILD_DIR)/test_statement.o
TEST_STATEMENT_TARGET = test_statement

TEST_CONDITION_SRC = $(TESTS_DIR)/test_condition.cpp
TEST_CONDITION_OBJ = $(BUILD_DIR)/test_condition.o
TEST_CONDITION_TARGET = test_condition

TEST_OPTIMIZER_SRC = $(TESTS_DIR)/test_optimizer.cpp
TEST_OPTIMIZER_OBJ = $(BUILD_DIR)/test_optimizer.o
TEST_OPTIMIZER_TARGET = test_optimizer

TEST_EXECUTOR_SRC  = $(TESTS_DIR)/test_executor.cpp
TEST_EXECUTOR_OBJ  = $(BUILD_DIR)/test_executor.o
TEST_EXECUTOR_TARGET = test_executor

TEST_MIDDLE_SRC    = $(TESTS_DIR)/test_middle.cpp
TEST_MIDDLE_OBJ    = $(BUILD_DIR)/test_middle.o
TEST_MIDDLE_TARGET = test_middle

# ============================================================
# 默认目标
# ============================================================
all: $(TARGET) $(TEST_PARSER_TARGET) $(TEST_STATEMENT_TARGET) \
     $(TEST_CONDITION_TARGET) $(TEST_OPTIMIZER_TARGET) \
     $(TEST_EXECUTOR_TARGET) $(TEST_MIDDLE_TARGET)
	@echo "✅ 所有程序构建完成"

# ============================================================
# 创建 build 目录
# ============================================================
$(BUILD_DIR):
	mkdir -p $(BUILD_DIR)

# ============================================================
# 生成 Flex/Bison
# ============================================================
$(YACC_OUT) $(YACC_HDR): $(YACC_SRC)
	$(YACC) -d -o $(YACC_OUT) $(YACC_SRC)

$(LEX_OUT) $(LEX_HDR): $(LEX_SRC) $(YACC_HDR)
	$(FLEX) --header-file=$(LEX_HDR) --outfile=$(LEX_OUT) $(LEX_SRC)

# ============================================================
# 编译 Parser
# ============================================================
$(AST_OBJ): $(AST_SRC) $(AST_HDR) | $(BUILD_DIR)
	$(CXX) $(CXXFLAGS) -c $< -o $@

$(LEX_OBJ): $(LEX_OUT) $(LEX_HDR) $(YACC_HDR) $(AST_HDR) | $(BUILD_DIR)
	$(CXX) $(CXXFLAGS) -Wno-unused-function -c $< -o $@

$(YACC_OBJ): $(YACC_OUT) $(YACC_HDR) $(AST_HDR) | $(BUILD_DIR)
	$(CXX) $(CXXFLAGS) -Wno-unused-function -c $< -o $@

# ============================================================
# 编译 Storage
# ============================================================
$(MOCK_OBJ): $(MOCK_SRC) $(MOCK_HDR) $(STORAGE_HDR) | $(BUILD_DIR)
	$(CXX) $(CXXFLAGS) -c $< -o $@

$(LEVELDB_OBJ): $(LEVELDB_SRC) $(LEVELDB_HDR) $(STORAGE_HDR) | $(BUILD_DIR)
	$(CXX) $(CXXFLAGS) -c $< -o $@

# ============================================================
# 编译 Core
# ============================================================
$(CONDITION_OBJ): $(CONDITION_SRC) $(CONDITION_HDR) $(STORAGE_HDR) | $(BUILD_DIR)
	$(CXX) $(CXXFLAGS) -c $< -o $@

$(STATEMENT_OBJ): $(STATEMENT_SRC) $(STATEMENT_HDR) $(AST_HDR) $(CONDITION_HDR) $(STORAGE_HDR) | $(BUILD_DIR)
	$(CXX) $(CXXFLAGS) -c $< -o $@

$(OPTIMIZER_OBJ): $(OPTIMIZER_SRC) $(OPTIMIZER_HDR) $(STATEMENT_HDR) $(CONDITION_HDR) $(STORAGE_HDR) | $(BUILD_DIR)
	$(CXX) $(CXXFLAGS) -c $< -o $@

$(EXECUTOR_OBJ): $(EXECUTOR_SRC) $(EXECUTOR_HDR) $(PLAN_HDR) $(CONDITION_HDR) $(STORAGE_HDR) | $(BUILD_DIR)
	$(CXX) $(CXXFLAGS) -c $< -o $@

# ============================================================
# 编译主程序
# ============================================================
$(MAIN_OBJ): $(MAIN_SRC) $(AST_HDR) $(STATEMENT_HDR) $(OPTIMIZER_HDR) $(EXECUTOR_HDR) $(STORAGE_HDR) | $(BUILD_DIR)
	$(CXX) $(CXXFLAGS) -c $< -o $@

# ============================================================
# 链接主程序
# ============================================================
$(TARGET): $(MAIN_OBJ) $(CORE_OBJS)
	$(CXX) $(CXXFLAGS) -o $@ $^ $(LDFLAGS)

# ============================================================
# 编译测试程序
# ============================================================
$(TEST_PARSER_OBJ): $(TEST_PARSER_SRC) $(AST_HDR) $(YACC_HDR) $(LEX_HDR) | $(BUILD_DIR)
	$(CXX) $(CXXFLAGS) -c $< -o $@

$(TEST_STATEMENT_OBJ): $(TEST_STATEMENT_SRC) $(AST_HDR) $(STATEMENT_HDR) $(STORAGE_HDR) | $(BUILD_DIR)
	$(CXX) $(CXXFLAGS) -c $< -o $@

$(TEST_CONDITION_OBJ): $(TEST_CONDITION_SRC) $(CONDITION_HDR) $(STORAGE_HDR) | $(BUILD_DIR)
	$(CXX) $(CXXFLAGS) -c $< -o $@

$(TEST_OPTIMIZER_OBJ): $(TEST_OPTIMIZER_SRC) $(STATEMENT_HDR) $(OPTIMIZER_HDR) $(STORAGE_HDR) | $(BUILD_DIR)
	$(CXX) $(CXXFLAGS) -c $< -o $@

$(TEST_EXECUTOR_OBJ): $(TEST_EXECUTOR_SRC) $(EXECUTOR_HDR) $(PLAN_HDR) $(STORAGE_HDR) | $(BUILD_DIR)
	$(CXX) $(CXXFLAGS) -c $< -o $@

$(TEST_MIDDLE_OBJ): $(TEST_MIDDLE_SRC) $(AST_HDR) $(STATEMENT_HDR) $(OPTIMIZER_HDR) $(EXECUTOR_HDR) $(STORAGE_HDR) | $(BUILD_DIR)
	$(CXX) $(CXXFLAGS) -c $< -o $@

# ============================================================
# 链接测试程序
# ============================================================
$(TEST_PARSER_TARGET): $(TEST_PARSER_OBJ) $(AST_OBJ) $(LEX_OBJ) $(YACC_OBJ)
	$(CXX) $(CXXFLAGS) -o $@ $^ $(LDFLAGS)

$(TEST_STATEMENT_TARGET): $(TEST_STATEMENT_OBJ) $(CORE_OBJS)
	$(CXX) $(CXXFLAGS) -o $@ $^ $(LDFLAGS)

$(TEST_CONDITION_TARGET): $(TEST_CONDITION_OBJ) $(CONDITION_OBJ) $(STORAGE_HDR)
	$(CXX) $(CXXFLAGS) -o $@ $^ $(LDFLAGS)

$(TEST_OPTIMIZER_TARGET): $(TEST_OPTIMIZER_OBJ) $(CORE_OBJS)
	$(CXX) $(CXXFLAGS) -o $@ $^ $(LDFLAGS)

$(TEST_EXECUTOR_TARGET): $(TEST_EXECUTOR_OBJ) $(CORE_OBJS)
	$(CXX) $(CXXFLAGS) -o $@ $^ $(LDFLAGS)

$(TEST_MIDDLE_TARGET): $(TEST_MIDDLE_OBJ) $(CORE_OBJS)
	$(CXX) $(CXXFLAGS) -o $@ $^ $(LDFLAGS)

# ============================================================
# 运行测试目标
# ============================================================
.PHONY: test test-parser test-statement test-condition \
        test-optimizer test-executor test-middle \
        test-all test-quick

# 运行所有测试
test: test-parser test-statement test-condition \
      test-optimizer test-executor test-middle
	@echo ""
	@echo "╔══════════════════════════════════════════╗"
	@echo "║     ✅ 所有测试通过                    ║"
	@echo "╚══════════════════════════════════════════╝"

# 快速测试（只运行核心测试）
test-quick: test-parser test-statement test-executor
	@echo ""
	@echo "✅ 快速测试完成"

# 分阶段测试
test-parser: $(TEST_PARSER_TARGET)
	@echo ""
	@echo "╔══════════════════════════════════════════╗"
	@echo "║     🧪 测试: Parser (词法/语法分析)    ║"
	@echo "╚══════════════════════════════════════════╝"
	./$(TEST_PARSER_TARGET)

test-statement: $(TEST_STATEMENT_TARGET)
	@echo ""
	@echo "╔══════════════════════════════════════════╗"
	@echo "║     🧪 测试: Statement Builder          ║"
	@echo "╚══════════════════════════════════════════╝"
	./$(TEST_STATEMENT_TARGET)

test-condition: $(TEST_CONDITION_TARGET)
	@echo ""
	@echo "╔══════════════════════════════════════════╗"
	@echo "║     🧪 测试: Condition (条件表达式)     ║"
	@echo "╚══════════════════════════════════════════╝"
	./$(TEST_CONDITION_TARGET)

test-optimizer: $(TEST_OPTIMIZER_TARGET)
	@echo ""
	@echo "╔══════════════════════════════════════════╗"
	@echo "║     🧪 测试: Optimizer (优化器)         ║"
	@echo "╚══════════════════════════════════════════╝"
	./$(TEST_OPTIMIZER_TARGET)

test-executor: $(TEST_EXECUTOR_TARGET)
	@echo ""
	@echo "╔══════════════════════════════════════════╗"
	@echo "║     🧪 测试: Executor (执行器)          ║"
	@echo "╚══════════════════════════════════════════╝"
	./$(TEST_EXECUTOR_TARGET)

test-middle: $(TEST_MIDDLE_TARGET)
	@echo ""
	@echo "╔══════════════════════════════════════════╗"
	@echo "║     🧪 测试: 完整中端流程               ║"
	@echo "╚══════════════════════════════════════════╝"
	./$(TEST_MIDDLE_TARGET)

# ============================================================
# 构建单个测试程序（不运行）
# ============================================================
.PHONY: build-parser build-statement build-condition \
        build-optimizer build-executor build-middle

build-parser: $(TEST_PARSER_TARGET)
	@echo "✅ Parser 测试构建完成"

build-statement: $(TEST_STATEMENT_TARGET)
	@echo "✅ Statement Builder 测试构建完成"

build-condition: $(TEST_CONDITION_TARGET)
	@echo "✅ Condition 测试构建完成"

build-optimizer: $(TEST_OPTIMIZER_TARGET)
	@echo "✅ Optimizer 测试构建完成"

build-executor: $(TEST_EXECUTOR_TARGET)
	@echo "✅ Executor 测试构建完成"

build-middle: $(TEST_MIDDLE_TARGET)
	@echo "✅ 完整中端测试构建完成"

# ============================================================
# 运行主程序
# ============================================================
run: $(TARGET)
	./$(TARGET)

# ============================================================
# 清理
# ============================================================
clean:
	rm -rf $(BUILD_DIR)
	rm -f $(TARGET) \
	      $(TEST_PARSER_TARGET) $(TEST_STATEMENT_TARGET) \
	      $(TEST_CONDITION_TARGET) $(TEST_OPTIMIZER_TARGET) \
	      $(TEST_EXECUTOR_TARGET) $(TEST_MIDDLE_TARGET) \
	      $(LEX_OUT) $(YACC_OUT) $(YACC_HDR) $(LEX_HDR)
	@echo "✅ 清理完成"

# ============================================================
# 完全清理
# ============================================================
distclean: clean
	rm -rf sql_db *.dSYM
	@echo "✅ 完全清理完成"

# ============================================================
# 帮助信息
# ============================================================
help:
	@echo "╔══════════════════════════════════════════╗"
	@echo "║     SQL 引擎 Makefile 帮助              ║"
	@echo "╚══════════════════════════════════════════╝"
	@echo ""
	@echo "构建目标："
	@echo "  make all              - 构建所有程序和测试"
	@echo "  make $(TARGET)        - 只构建主程序"
	@echo "  make build-parser     - 构建 Parser 测试"
	@echo "  make build-statement  - 构建 Statement 测试"
	@echo "  make build-condition  - 构建 Condition 测试"
	@echo "  make build-optimizer  - 构建 Optimizer 测试"
	@echo "  make build-executor   - 构建 Executor 测试"
	@echo "  make build-middle     - 构建完整中端测试"
	@echo ""
	@echo "运行测试："
	@echo "  make test             - 运行所有测试"
	@echo "  make test-quick       - 运行快速测试"
	@echo "  make test-parser      - 运行 Parser 测试"
	@echo "  make test-statement   - 运行 Statement Builder 测试"
	@echo "  make test-condition   - 运行 Condition 测试"
	@echo "  make test-optimizer   - 运行 Optimizer 测试"
	@echo "  make test-executor    - 运行 Executor 测试"
	@echo "  make test-middle      - 运行完整中端测试"
	@echo ""
	@echo "其他："
	@echo "  make run              - 运行主程序"
	@echo "  make clean            - 清理构建文件"
	@echo "  make distclean        - 完全清理（含数据库）"
	@echo "  make help             - 显示此帮助信息"

.PHONY: all clean distclean help run \
        test test-quick test-parser test-statement test-condition \
        test-optimizer test-executor test-middle \
        build-parser build-statement build-condition \
        build-optimizer build-executor build-middle