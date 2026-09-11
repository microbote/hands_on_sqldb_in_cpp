// tests/test_parser/main.cpp
#include "test_framework.h"

int main(int argc, char** argv) {
    // parser 库默认不打印；这里也不注册回调，保持测试输出干净
    return test::run_all(argc, argv);
}
