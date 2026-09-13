# test_storage 模块（存储层测试）

`tests/test_storage/` 是**存储层**（`kv::KVStore` / `kv::KVEngine` / `Iterator` /
`WriteBatch`）的测试，19 个用例。它是从被删掉的旧手工 demo
（`tests/test_mock_engine.cpp`、`tests/test_leveldb_engine.cpp`）里**把用例抽出来
重建**的：那些 demo 是 `main()` + `cout` 的打印脚本，既不能回归（没有断言），
也只跑一个引擎。

## 一、铁律：凡是不涉及引擎特性的用例，两个引擎都跑

`storage_test_util.h` 提供 `run_on_both(name, body)`：同一份断言先在
Mock 上跑，再在 LevelDB 上跑（LevelDB 每次用全新目录，`/tmp/sqldb_storage_test_*`）。
只跑一个引擎的情况必须是有理由的：`run_on_mock` 只用于 Mock 特有的故障注入，
`open_leveldb_at` 只用于"重开同一个库"的持久化用例。

这条规则不是洁癖：第 2 节里三个引擎行为差异全都是**新测试第一次跑就抓出来的**。

## 二、新测试抓出来并修掉的三个"两引擎行为不一致"

| # | 现象 | 修法 |
|---|------|------|
| 1 | **删不存在的 key**：Mock 返回 `NotFound`，LevelDB 返回 `OK`（leveldb 的 `Delete` 对不存在的 key 就是成功） | `LevelDBStore::remove` 先 `Get` 一遍，缺失返回 `NotFound`（多一次读，换两个引擎同一套语义）。事务里的删除仍然是幂等的（缓冲里的删除 = 待提交的 op） |
| 2 | **一条非法 op 的批量**：Mock 整批拒绝（`InvalidArgument`，一条都不落），LevelDB 把非法 `remove_range` 静默当空操作并返回 `OK` | `LevelDBStore::apply_batch_locked` 改成**两趟**（先整体校验、再整体应用），与 Mock 的注释和语义对齐 |
| 3 | **`seek()` 与区间边界**：Mock 把 key 夹进区间（区间之前 → 区间第一条），LevelDB 直接 `Seek` 到区间外然后判定无效；反向 seek 越过上界时也一样 | `LevelDBIterator::seek` 正向先夹到 `range.start`（与 `seek_to_first` 一致），反向当 `key >= range.end` 时夹到区间最后一条 |

顺带清掉一个库内打印 + 一个反直觉 API：`Iterator::for_each` 原来在回调返回
`false` 时 `fprintf(stderr, "callback error ...")` 并且**继续遍历**（除非再传
`break_if_error`）。现在语义是"返回 false = 提前停止、库代码不打印"，
`break_if_error` 参数去掉（调用方：只有测试）。

## 三、用例

| 文件 | 覆盖 |
|------|------|
| `test_kv_basics.cpp`（6） | put/get/覆盖写/remove 往返；**缺失 key 的三条语义（get/exists/remove 两次）**；覆盖写不涨 key 数；`write_batch` 的 put+remove；非法 op 整批拒绝；`get_batch` 的两种缺失策略；`remove_range` 左闭右开 |
| `test_scan.cpp`（7） | 全扫 / `[b,d)` 半开区间 / 前缀扫描 / 反向降序 / `seek`+区间夹取、`seek_to_last`、`seek_to_first`、`prev` / 空库与空区间 / `for_each` 提前停止 / 迭代器耗尽后 `next`/`prev` 是 no-op |
| `test_lifecycle.cpp`（5） | `open/close/重复 close`、关闭后连接立即失效、连接与 Store 的从属关系、两条连接共享存储但事务各管各的、**leveldb 重开同一目录数据还在**（提交即持久）、Mock 故障注入只挡批量写 |

（旧的 `test_relation.cpp` / `test_query.cpp` / `test_optimizer.cpp` /
`test_rewriter.cpp` / `test_condition.cpp` / `test_pushdown_not.cpp` /
`test_executor.cpp` / `test_row_builder.cpp` / `test_middle*.cpp` 全部删除：
它们引用的旧模块（`db_manager`、`query/`、`relation/value.cpp` …）早已不存在，
对应的用例已经由 `test_relation` / `test_planner` / `test_statement` /
`test_executor` / `test_sql_types` 覆盖，没有可借鉴的遗漏项。）

## 四、跑法

```bash
cmake --build build -j4 --target test_storage && ./build/run_tests/test_storage
make storage-test          # 等价
ctest --test-dir build -R test_storage --output-on-failure
```

## 五、下一步（存储层）

- 写事务工作集（行锁）：**暂不做**，理由见 `storage/kv_engine/readme.md` 第 7 节
  （单写者已经给出更强的互斥；行锁要等"多写者 + 快照读"一起上，否则只是幻读陷阱）。
- 只读事务的可重复读（读锁持有到事务结束 / `leveldb::Snapshot`）二选一。
- 大事务 spill、`DeleteRange`（当前 leveldb 版本没有，整段删在提交时展开）。
