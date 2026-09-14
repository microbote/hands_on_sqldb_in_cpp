# SQL Engine with LevelDB Backend

English version: [README.en.md](README.en.md)

一个从零写的轻量 SQL 引擎：Flex/Bison 词法语法、自己的类型系统与 schema、
基于主键有序编码的区间扫描、Volcano 执行器、悲观单写者事务，底层是
LevelDB（另有一份内存 Mock 引擎，用于测试与开发）。

```
SQL 文本
  │ parser/        Flex+Bison -> AST（位置/高亮都从这里来）
  │ statement/     AST -> sql::Query（结构转换 + 语义校验）
  │ planner/       重写 -> 优化（主键区间 + 残余谓词 + 成本模型）-> 计划树
  │ executor/      Volcano 算子 -> ResultCursor（客户端拿到的是 sql::Cursor）
  │ relation/      Catalog / Table / Cursor（表视图与元数据）
  │ storage/       KVStore + KVEngine（连接）、TxBuffer、Mock / LevelDB 实现
  └ session/       把上面串起来：一条 SQL -> 一个游标（CLI 的执行入口）
```

## 依赖

- C++23 编译器（本仓库用 `clang++-mp-23` 验证）
- CMake ≥ 3.20、Flex、Bison ≥ 3.0（`/usr/local/opt/bison/bin/bison`）
- LevelDB 开发库、fmt；readline（CLI 用）

## 构建与测试

```bash
cmake -S . -B build
cmake --build build -j4
ctest --test-dir build --output-on-failure
```

`Makefile` 只是这套 CMake 的薄包装（`make build` / `make test` /
`make storage-test` …）。产物：`build/lib*.a`、`build/sqldb`（CLI）、
`build/run_tests/test_*`（每个模块一个可执行文件）。

测试分模块放在 `tests/test_<模块>/`，**凡是不涉及引擎特性的用例都在
Mock 与 LevelDB 上各跑一遍**（两个引擎语义必须一致）。

## 使用

```bash
./build/sqldb                          # 内存引擎，交互式
./build/sqldb --engine=leveldb --path=./mydb
./build/sqldb -e "SELECT * FROM users LIMIT 3;"
./build/sqldb script.sql               # 跑脚本
```

```
shop> CREATE TABLE users (id INT PRIMARY KEY, name VARCHAR(16) NOT NULL);
OK
shop> INSERT INTO users (id, name) VALUES (1, 'a'), (2, 'b');
OK, 2 rows affected
shop> SELECT id, name FROM users WHERE id >= 1 ORDER BY id DESC;
id  name
--  ----
2   b
1   a
(2 rows)
```

支持：`CREATE/DROP DATABASE`、`CREATE/DROP TABLE`、`SELECT`（`WHERE`/`ORDER BY`/
`LIMIT`/`OFFSET`）、`INSERT`（多行 VALUES）、`UPDATE`、`DELETE`、`USE`、
`EXPLAIN [ANALYZE]`、事务 `BEGIN/COMMIT/ROLLBACK`（含 `START TRANSACTION`/
`END`/`ABORT`）。细节见 `client/README.md` 与各模块 README。

两个前端（共用同一套 REPL 与协议，只是 transport 不同）：

```bash
./build/sqldb                # 本地：进程内引擎（脚本/嵌入/离线）
./build/sqldb-client --host=127.0.0.1 --port=5433   # 远程：连 sqldb-server
```

## 文档

每个模块的 `README.md` / `readme.md` 是**该模块的最新说明**（职责边界、
接口约定、已知缺口、踩过的坑），改代码时一起改：

每个说明都有**英文镜像 `*.en.md`**（例如 `parser/README.en.md`）；中文版是
源头（信息最全），行为变化时两份都要改。

| 目录 | 内容 |
|------|------|
| `parser/README.md` | 词法/语法、AST 节点、支持矩阵与保留字 |
| `sql_types/README.md` | 类型系统、Value/Key 编码、KeyRange（逻辑区间） |
| `statement/README.md` | AST -> Query 的转换与语义校验 |
| `planner/readme.md` | 重写 / 优化 / 计划树 / 成本模型 |
| `executor/readme.md` | Volcano 算子、结果游标、EXPLAIN 统计 |
| `storage/kv_engine/readme.md` | KVStore/KVEngine、事务缓冲、两引擎一致性 |
| `client/README.md` | 两个客户端（本地 `sqldb` / 远程 `sqldb-client`）的用法、元命令、EXPLAIN 输出 |
| `server/README.md` | 服务端 `sqldb-server`：产物目录（`build/svr/{bin,etc}`）、配置、**日志**、路由 |
| `raft/DESIGN.md` | **设计文档**：sqldb 的 Multi-Raft 方案、两条已拍板决定、分片/事务规则、分阶段计划；P0 core + P1（单 group 打通 SQL，含 TCP transport 与 server 接线）已落地 |

各模块的改动记录（含设计取舍与踩坑）在各测试目录的
`tests/test_<模块>/codex_check_issues.md`。
