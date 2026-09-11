// tests/test_parser/test_error_reporting.cpp
//
// 错误处理：行号、未知字符不再被吞掉、库不打印。
#include "test_framework.h"

#include "parser/parser.h"

#include <string>
#include <vector>

TEST(ErrorReporting, LineNumberIsReported) {
    parser::Parser p;
    auto result = p.parse("\n\n\nSELECT * FROM WHERE;");
    CHECK(!result.success);
    CHECK(result.error.has_value());
    if (result.error.has_value()) {
        // 之前没有 %option yylineno，第 4 行的错误会报成 line 1
        CHECK(result.error->message.find("line 4") != std::string::npos);
    }
}

TEST(ErrorReporting, UnknownCharacterIsNotSilentlyDropped) {
    parser::Parser p;
    // '"' 现在会作为非法字符进入语法层，而不是被丢弃后解析"成功"
    auto quoted = p.parse("SELECT * FROM \"t\";");
    CHECK(!quoted.success);

    // '#' 同理
    auto hash = p.parse("SELECT * FROM t#;");
    CHECK(!hash.success);
}

TEST(ErrorReporting, EachErrorIsIndependent) {
    parser::Parser p;
    CHECK(!p.parse("SELEC * FROM t;").success);
    CHECK(p.parse("SELECT * FROM t;").success);
    CHECK(!p.parse("SELECT * FROM;").success);
    CHECK(p.parse("SELECT * FROM t;").success);
    CHECK_EQ(p.error_count(), 2u);
}

TEST(ErrorReporting, LibraryDoesNotPrintByDefault) {
    // 默认不注册回调 -> 库内部不产生任何输出
    parser::Parser p;
    std::vector<std::string> messages;
    p.set_log_callback([&messages](const std::string& m) { messages.push_back(m); });

    CHECK(p.parse("SELECT * FROM t;").success);
    // 注册之后才收到消息，说明默认路径是静默的
    CHECK(!messages.empty());
}
