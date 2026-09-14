#!/bin/bash
# db_dev/run_tests.sh

set -e

# 配置（如果 build 不存在）
if [ ! -f "build/CMakeCache.txt" ]; then
    echo "Configuring..."
    cmake --preset llvm-debug
fi

# 构建
echo "Building..."
cmake --build build -j3

# 运行
echo "Running tests..."
echo "cd build"
