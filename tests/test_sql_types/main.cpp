// db_dev/tests/test_sql_types/main.cpp
#include "test_framework.h"

// 声明外部测试注册函数（各测试文件的注册在全局构造函数中完成）
// 不需要这里做任何事，只要 main 函数即可

int main() {
    return test::run_all();
}