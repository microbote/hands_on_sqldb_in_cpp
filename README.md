# SQL Engine with LevelDB Backend

基于 LevelDB 的轻量级 SQL 引擎，采用 Lex/Bison 生成语法树，支持 SELECT、INSERT、UPDATE、DELETE、USE DATABASE 等语句。

## 架构

```
SQL 文本 → Lex(词法) → Bison(语法) → AST → Statement → PlanNode → Executor → LevelDB
```

## 依赖

- C++20 编译器（g++ 10+ 或 clang++ 14+）
- LevelDB 开发库
- fmt 格式化库
- GNU Readline
- Flex + Bison

## 构建

```bash
make
```

## 使用

```
$ ./sql_engine
========================================
  SQL Engine Shell (C++20 + fmt)
  Commands: open, close, use, select, insert, update, delete, scan, batch, history, quit
========================================
(sql> open ./mydb
OK: opened ./mydb
(mydb) sql> ...
```

## 项目文件

| 文件 | 说明 |
|------|------|
| sql.l | Lex 词法分析器 |
| sql.y | Bison 语法分析器 |
| ast.h | AST 节点定义 |
| statement.h | 语句树类型定义 |
| plan.h | 执行计划节点定义 |
| catalog.h | 数据库目录/元数据 |
| statement_builder.cpp | AST → Statement 转换 |
| planner.cpp | Statement → PlanNode 转换 |
| executor.h/cpp | 执行器 |
| main.cpp | 主程序入口 |
| Makefile | 构建配置 |
