# ============================================================
# 编译器和编译选项
# ============================================================
CXX      = /usr/local/opt/llvm/bin/clang++
# 生成的 lex.yy.c / parser.tab.c 按 C 编译（parser 保持 C 风格，
# 也让 yyerror/yyparse 等符号保持 C 链接）
CC       = /usr/local/opt/llvm/bin/clang
CFLAGS   = -std=c11 -g \
           -I$(SRC_DIR) \
           -I$(PARSER_DIR)
CXXFLAGS = -std=c++23 -stdlib=libc++ -g -Wall -Wextra \
           -I$(SRC_DIR) \
           -I$(PARSER_DIR) \
           -I../Debug/include \
           -I/usr/local/include \
           -I/usr/local/opt/readline/include \
					 -I/usr/local/Cellar/llvm/20.1.5/bin/../include/c++/v1

LDFLAGS  = -L../Debug/lib -lleveldb \
           -L/usr/local/opt/readline/lib -lreadline \
           -L/usr/local/lib -lfmt \
           -lpthread -l snappy \
           -L/usr/local/opt/llvm/lib/c++ -lc++abi -lc++	

FLEX     = flex
YACC     = /usr/local/opt/bison/bin/bison -t

# ============================================================
# 目录路径
# ============================================================
SRC_DIR      = .
PARSER_DIR   = $(SRC_DIR)/parser
QUERY_DIR    = $(SRC_DIR)/query
RELATION_DIR = $(SRC_DIR)/relation
STORAGE_DIR  = $(SRC_DIR)/storage
TESTS_DIR    = $(SRC_DIR)/tests
BUILD_DIR    = build

# ============================================================
# Parser 文件 (Flex/Bison)
# ============================================================
LEX_SRC  = $(PARSER_DIR)/sql.l
YACC_SRC = $(PARSER_DIR)/sql.y
LEX_OUT  = $(PARSER_DIR)/lex.yy.c
LEX_HDR  = $(PARSER_DIR)/lex.yy.h
YACC_OUT = $(PARSER_DIR)/parser.tab.c
YACC_HDR = $(PARSER_DIR)/parser.tab.h
AST_HDR  = $(PARSER_DIR)/ast.h

# ============================================================
# 源文件（完整路径）
# ============================================================
# Parser
PARSER_HDR = $(wildcard $(PARSER_DIR)/*.h)
PARSER_SRCS    = $(PARSER_DIR)/ast.cpp $(PARSER_DIR)/parser.cpp
GENERATED_SRCS = $(PARSER_DIR)/lex.yy.c $(PARSER_DIR)/parser.tab.c

# Storage
STORAGE_HDR    = $(wildcard $(STORAGE_DIR)/*/*.h)
STORAGE_SRCS   = $(STORAGE_DIR)/kv_engine/kv_factory.cpp \
                 $(STORAGE_DIR)/mock_engine/mock_engine.cpp \
                 $(STORAGE_DIR)/leveldb_engine/leveldb_engine.cpp

# Relation
RELATION_HDR   = $(wildcard $(RELATION_DIR)/*.h) $(STORAGE_HDR)
RELATION_SRCS  = $(RELATION_DIR)/value.cpp \
                 $(RELATION_DIR)/schema.cpp \
								 $(RELATION_DIR)/cursor.cpp \
                 $(RELATION_DIR)/row.cpp \
                 $(RELATION_DIR)/table.cpp \
                 $(RELATION_DIR)/database.cpp \
                 $(RELATION_DIR)/database_manager.cpp

# Tests
# parser 测试在 tests/test_parser/、statement 测试在 tests/test_statement/、
# sql_types 测试在 tests/test_sql_types/，三者都走 CMake/ctest
# （见下面的 sql-types-test / parser-test / statement-test / planner-test /
#   relation-test / executor-test / cmake-test 目标）。
#   session-test / cli 也一样走 CMake（见文件末尾）。
TESTS_SRCS     = $(TESTS_DIR)/test_condition.cpp \
                 $(TESTS_DIR)/test_optimizer.cpp \
                 $(TESTS_DIR)/test_executor.cpp \
                 $(TESTS_DIR)/test_middle.cpp \
                 $(TESTS_DIR)/test_mock_engine.cpp \
                 $(TESTS_DIR)/test_leveldb_engine.cpp \
                 $(TESTS_DIR)/test_relation.cpp

QUERY_HDR      = $(wildcard $(QUERY_DIR)/*.h)
QUERY_SRCS     = $(QUERY_DIR)/statement/condition.cpp

# Main
MAIN_SRC       = $(SRC_DIR)/main.cpp

# ============================================================
# 对象文件（从源文件路径生成）
# ============================================================
# Parser 对象
PARSER_OBJS = $(patsubst $(PARSER_DIR)/%.c,$(BUILD_DIR)/%.o,$(GENERATED_SRCS)) \
							$(patsubst $(PARSER_DIR)/%.cpp,$(BUILD_DIR)/%.o,$(PARSER_SRCS)) 

# Storage 对象
STORAGE_OBJS = $(patsubst $(STORAGE_DIR)/%/%.cpp,$(BUILD_DIR)/%.o,$(STORAGE_SRCS))

# Relation 对象
RELATION_OBJS = $(patsubst $(RELATION_DIR)/%.cpp,$(BUILD_DIR)/%.o,$(RELATION_SRCS))

# Query 对象
QUERY_OBJS = $(patsubst $(QUERY_DIR)/%/%.cpp,$(BUILD_DIR)/%.o,$(QUERY_SRCS))

# Tests 对象
TESTS_OBJS = $(patsubst $(TESTS_DIR)/%.cpp,$(BUILD_DIR)/%.o,$(TESTS_SRCS))

# Main 对象
MAIN_OBJ = $(BUILD_DIR)/main.o

# ============================================================
# 核心对象（所有程序共享）
# ============================================================
CORE_OBJS = $(PARSER_OBJS) $(STORAGE_OBJS) $(RELATION_OBJS) \
            $(QUERY_OBJS)

# ============================================================
# 测试目标
# ============================================================
TEST_TARGETS = test_condition \
               test_optimizer test_executor test_middle \
               test_mock_engine test_leveldb_engine test_relation

TARGET = sql_engine

# ============================================================
# 默认目标
# ============================================================
all: $(TARGET) $(TEST_TARGETS)
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
# 编译规则（完整路径）
# ============================================================

# ---- Parser 的 .cpp ----
$(BUILD_DIR)/ast.o: $(PARSER_DIR)/ast.cpp $(AST_HDR) | $(BUILD_DIR)
	$(CXX) $(CXXFLAGS) -c $< -o $@

$(BUILD_DIR)/parser.o: $(PARSER_DIR)/parser.cpp $(PARSER_HDR) | $(BUILD_DIR)
	$(CXX) $(CXXFLAGS) -c $< -o $@

# ---- Parser 的 .c（Flex/Bison 生成） ----
$(BUILD_DIR)/lex.yy.o: $(PARSER_DIR)/lex.yy.c $(LEX_HDR) $(YACC_HDR) $(AST_HDR) | $(BUILD_DIR)
	$(CC) $(CFLAGS) -Wno-unused-function -c $< -o $@

$(BUILD_DIR)/parser.tab.o: $(PARSER_DIR)/parser.tab.c $(YACC_HDR) $(AST_HDR) | $(BUILD_DIR)
	$(CC) $(CFLAGS) -Wno-unused-function -c $< -o $@

# ---- Storage ----
$(BUILD_DIR)/kv_factory.o: $(STORAGE_DIR)/kv_engine/kv_factory.cpp $(STORAGE_HDR) | $(BUILD_DIR)
	$(CXX) $(CXXFLAGS) -c $< -o $@

$(BUILD_DIR)/mock_engine.o: $(STORAGE_DIR)/mock_engine/mock_engine.cpp $(STORAGE_HDR) | $(BUILD_DIR)
	$(CXX) $(CXXFLAGS) -c $< -o $@

$(BUILD_DIR)/leveldb_engine.o: $(STORAGE_DIR)/leveldb_engine/leveldb_engine.cpp $(STORAGE_HDR) | $(BUILD_DIR)
	$(CXX) $(CXXFLAGS) -c $< -o $@

# ---- Relation ----
$(BUILD_DIR)/value.o: $(RELATION_DIR)/value.cpp $(RELATION_HDR) | $(BUILD_DIR)
	$(CXX) $(CXXFLAGS) -c $< -o $@

$(BUILD_DIR)/schema.o: $(RELATION_DIR)/schema.cpp $(RELATION_HDR) | $(BUILD_DIR)
	$(CXX) $(CXXFLAGS) -c $< -o $@

$(BUILD_DIR)/row.o: $(RELATION_DIR)/row.cpp $(RELATION_HDR) | $(BUILD_DIR)
	$(CXX) $(CXXFLAGS) -c $< -o $@

$(BUILD_DIR)/cursor.o: $(RELATION_DIR)/cursor.cpp $(RELATION_HDR) | $(BUILD_DIR)
	$(CXX) $(CXXFLAGS) -c $< -o $@

$(BUILD_DIR)/table.o: $(RELATION_DIR)/table.cpp $(RELATION_HDR) | $(BUILD_DIR)
	$(CXX) $(CXXFLAGS) -c $< -o $@

$(BUILD_DIR)/database.o: $(RELATION_DIR)/database.cpp $(RELATION_HDR) | $(BUILD_DIR)
	$(CXX) $(CXXFLAGS) -c $< -o $@

$(BUILD_DIR)/database_manager.o: $(RELATION_DIR)/database_manager.cpp $(RELATION_HDR) | $(BUILD_DIR)
	$(CXX) $(CXXFLAGS) -c $< -o $@

$(BUILD_DIR)/condition.o : $(QUERY_DIR)/statement/condition.cpp $(QUERY_HDR) $(RELATION_HDR) | $(BUILD_DIR)
	$(CXX) $(CXXFLAGS) -c $< -o $@

# ---- Tests ----
$(BUILD_DIR)/test_optimizer.o: $(TESTS_DIR)/test_optimizer.cpp $(QUERY_DIR)/optimizer.h $(RELATION_DIR)/database_manager.h | $(BUILD_DIR)
	$(CXX) $(CXXFLAGS) -c $< -o $@

$(BUILD_DIR)/test_executor.o: $(TESTS_DIR)/test_executor.cpp $(QUERY_DIR)/executor.h $(RELATION_DIR)/database_manager.h | $(BUILD_DIR)
	$(CXX) $(CXXFLAGS) -c $< -o $@

$(BUILD_DIR)/test_middle.o: $(TESTS_DIR)/test_middle.cpp $(AST_HDR) $(QUERY_DIR)/statement_builder.h $(QUERY_DIR)/optimizer.h $(QUERY_DIR)/executor.h $(RELATION_DIR)/database_manager.h | $(BUILD_DIR)
	$(CXX) $(CXXFLAGS) -c $< -o $@

$(BUILD_DIR)/test_mock_engine.o: $(TESTS_DIR)/test_mock_engine.cpp $(STORAGE_HDR) | $(BUILD_DIR)
	$(CXX) $(CXXFLAGS) -c $< -o $@

$(BUILD_DIR)/test_leveldb_engine.o: $(TESTS_DIR)/test_leveldb_engine.cpp $(STORAGE_HDR) | $(BUILD_DIR)
	$(CXX) $(CXXFLAGS) -c $< -o $@

$(BUILD_DIR)/test_relation.o: $(TESTS_DIR)/test_relation.cpp $(RELATION_HDR) | $(BUILD_DIR)
	$(CXX) $(CXXFLAGS) -c $< -o $@

$(BUILD_DIR)/test_condition.o: $(TESTS_DIR)/test_condition.cpp $(QUERY_HDR) $(RELATION_HDR) | $(BUILD_DIR)
	$(CXX) $(CXXFLAGS) -c $< -o $@

# ---- Main ----
$(BUILD_DIR)/main.o: $(MAIN_SRC) $(AST_HDR) $(RELATION_DIR)/sql_relation.h $(STORAGE_DIR)/kv_engine/kv_engine.h | $(BUILD_DIR)
	$(CXX) $(CXXFLAGS) -c $< -o $@

# ============================================================
# 链接主程序
# ============================================================
$(TARGET): $(MAIN_OBJ) $(CORE_OBJS)
	$(CXX) $(CXXFLAGS) -o $@ $^ $(LDFLAGS)

# ============================================================
# 链接测试程序
# ============================================================
test_condition: $(BUILD_DIR)/test_condition.o $(CORE_OBJS)
	$(CXX) $(CXXFLAGS) -o $@ $^ $(LDFLAGS)

test_optimizer: $(BUILD_DIR)/test_optimizer.o $(CORE_OBJS)
	$(CXX) $(CXXFLAGS) -o $@ $^ $(LDFLAGS)

test_executor: $(BUILD_DIR)/test_executor.o $(CORE_OBJS)
	$(CXX) $(CXXFLAGS) -o $@ $^ $(LDFLAGS)

test_middle: $(BUILD_DIR)/test_middle.o $(CORE_OBJS)
	$(CXX) $(CXXFLAGS) -o $@ $^ $(LDFLAGS)

test_mock_engine: $(BUILD_DIR)/test_mock_engine.o $(STORAGE_OBJS)
	$(CXX) $(CXXFLAGS) -o $@ $^ $(LDFLAGS)

test_leveldb_engine: $(BUILD_DIR)/test_leveldb_engine.o $(STORAGE_OBJS)
	$(CXX) $(CXXFLAGS) -o $@ $^ $(LDFLAGS)

test_relation: $(BUILD_DIR)/test_relation.o $(CORE_OBJS)
	$(CXX) $(CXXFLAGS) -o $@ $^ $(LDFLAGS)

# ============================================================
# 运行测试目标
# ============================================================
.PHONY: test test-quick $(addprefix test-,$(TEST_TARGETS))

test: $(TEST_TARGETS)
	@echo ""
	@echo "╔══════════════════════════════════════════╗"
	@echo "║     ✅ 所有测试通过                    ║"
	@echo "╚══════════════════════════════════════════╝"

test-quick: test_relation
	@echo ""
	@echo "✅ 快速测试完成"

define TEST_RULE
test-$(1):
	@echo ""
	@echo "╔══════════════════════════════════════════╗"
	@echo "║     🧪 测试: $(1)                      ║"
	@echo "╚══════════════════════════════════════════╝"
	./$(1)
endef

$(foreach t,$(TEST_TARGETS),$(eval $(call TEST_RULE,$(t))))

# ============================================================
# sql_types 模块（新构建系统走 CMake，与上面的 legacy 目标解耦）
# ============================================================
.PHONY: sql-types sql-types-test parser-test statement-test planner-test \
        relation-test executor-test session-test cli cmake-test

sql-types:
	cmake --build $(BUILD_DIR) --target sql_types

sql-types-test:
	cmake --build $(BUILD_DIR) -j4
	./$(BUILD_DIR)/run_tests/test_sql_types

parser-test:
	cmake --build $(BUILD_DIR) -j4
	./$(BUILD_DIR)/run_tests/test_parser

statement-test:
	cmake --build $(BUILD_DIR) -j4
	./$(BUILD_DIR)/run_tests/test_statement

planner-test:
	cmake --build $(BUILD_DIR) -j4
	./$(BUILD_DIR)/run_tests/test_planner

relation-test:
	cmake --build $(BUILD_DIR) -j4
	./$(BUILD_DIR)/run_tests/test_relation

executor-test:
	cmake --build $(BUILD_DIR) -j4
	./$(BUILD_DIR)/run_tests/test_executor

session-test:
	cmake --build $(BUILD_DIR) -j4
	./$(BUILD_DIR)/run_tests/test_session

cli:
	cmake --build $(BUILD_DIR) -j4 --target sqldb
	@echo "✅ $(BUILD_DIR)/sqldb 可用（./$(BUILD_DIR)/sqldb --help）"

# 跑 CMake/ctest 里的全部测试（sql_types + parser + statement + planner + relation）
cmake-test:
	cmake --build $(BUILD_DIR) -j4
	ctest --test-dir $(BUILD_DIR) --output-on-failure

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
	rm -f $(TARGET) $(TEST_TARGETS) $(LEX_OUT) $(YACC_OUT) $(YACC_HDR) $(LEX_HDR)
	@echo "✅ 清理完成"

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
	@echo ""
	@echo "运行测试："
	@echo "  make test             - 运行所有测试"
	@echo "  make test-quick       - 运行快速测试 (parser, statement, relation)"
	@echo "  make test-parser      - 运行 Parser 测试"
	@echo "  make test-statement   - 运行 Statement 测试"
	@echo "  make test-condition   - 运行 Condition 测试"
	@echo "  make test-optimizer   - 运行 Optimizer 测试"
	@echo "  make test-executor    - 运行 Executor 测试"
	@echo "  make test-middle      - 运行完整中端测试"
	@echo "  make test-relation    - 运行 Relation 测试"
	@echo "  make test-mock-engine - 运行 Mock Engine 测试"
	@echo "  make test-leveldb-engine - 运行 LevelDB Engine 测试"
	@echo ""
	@echo "其他："
	@echo "  make run              - 运行主程序"
	@echo "  make clean            - 清理构建文件"
	@echo "  make distclean        - 完全清理（含数据库）"
	@echo "  make help             - 显示此帮助信息"

.PHONY: all clean distclean help run test test-quick
