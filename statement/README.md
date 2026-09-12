# statement 模块

把 parser 产出的 AST 变成 **sql_types 的 `sql::Query`**，并在执行前做语义校验。

```
SQL 文本 --parser--> AST --StatementBuilder--> sql::Query --StatementValidator--> OK / StmtError
                                                              (查 sql::Catalog)
```

## 两个类的职责

| 类 | 输入 | 输出 | 做什么 | 不做什么 |
|----|------|------|--------|----------|
| `StatementBuilder` | `const ASTNode*` | `std::expected<sql::Query, StmtError>` | 节点结构转换、结构校验（字段是否齐全、值节点是否合法）、SQL 语义规范化（如 PRIMARY KEY ⇒ NOT NULL） | 不查 Catalog、不碰存储、**不接管 AST 所有权、不改 AST**（接口是 `const ASTNode*`，生命周期由调用方管理） |
| `StatementValidator` | `const sql::Query&` + `const sql::Catalog&` | `std::expected<void, StmtError>` | 库/表/列是否存在、值类型族与取值范围、主键约束、DDL 语义（重复建库/建表、删不存在的对象） | 不修改数据；**校验通过 ≠ 已执行** |

辅助：`statement/source_span.h`（位置格式化、caret 渲染、名字→位置解析）。

调用方（未来的执行器/shell）需要自己把通过校验的 DDL 落到 Catalog 上，
或者走上层事务接口 —— validator 是只读的。

两个类都**不保存错误状态**：错误码与错误信息随返回值一起走，
因此同一个 builder/validator 可以反复调用、并发使用，
也不会出现"上一次的错误粘住下一次调用"（早期用成员 `error_` 的版本就有这个 bug）。

## 错误处理（`stmt_defs.h`）

```cpp
enum class StmtErrorCode : uint8_t {
  OK = 0,
  // 校验类 1..49
  UNKNOWN_STMT_TYPE, VALIDATOR_NOT_INIT, DATABASE_NOT_FOUND,
  DATABASE_ALREADY_EXISTS, TABLE_NOT_FOUND, TABLE_ALREADY_EXISTS,
  COLUMN_NOT_FOUND, COLUMN_COUNT_MISMATCH, COLUMN_TYPE_MISMATCH,
  VALUE_OUT_OF_RANGE, COLUMN_ATTR_NULL_MISMATCH, DUPLICATE_COLUMN,
  DUPLICATE_PRIMARY_KEY, NO_PRIMARY_KEY, INVALID_SCHEMA, EMPTY_STATEMENT,
  // 构建类 99..
  AST_IS_NULL = 99, UNKNOWN_AST_Type, INVALID_AST_NODE, UNSUPPORTED_AST_NODE,
};

struct StmtError {              // 错误是"值"：码 + 上下文信息
  StmtErrorCode code = StmtErrorCode::OK;
  std::string message;
  SSpan span;                   // 出错位置（来自 AST；未知时 sspan_valid() == 0）
  bool ok() const;
  explicit operator bool() const;
  std::string to_string() const;   // message 为空时退回 stmt_error_message(code)
};
```

用法：

```cpp
stmt::StatementBuilder builder;
auto query = builder.build(ast);                 // expected<Query, StmtError>
if (!query) {
  log(query.error().code, query.error().message);
  return;
}
stmt::StatementValidator validator(catalog);
if (auto ok = validator.validate(*query); !ok) { // expected<void, StmtError>
  log(ok.error().to_string());
}
```

## 错误定位（source span）

错误不只有"哪条语句错了"，还能指出"**哪个名字/字面量错了**"。链路：

```
sql.l（%option yylineno + 自己维护列号）
   │  yylloc{first_line, first_column, last_line, last_column}
   ▼
sql.y（%locations；AST_SET_SPAN($$, @$) / @n）
   │  ASTNode::span、SelectNode::table_span、DatabaseNode::db_span ...
   ▼
StmtError.span（builder 直接取节点位置；validator 用 resolver 按名字查位置）
   │
   ▼
sspan_caret(sql, span) → CLI 展示
```

用法（validator 想要位置就多传一个 resolver）：

```cpp
stmt::StatementBuilder builder;
auto query = builder.build(ast.get());

stmt::StatementValidator validator(catalog, stmt::make_span_resolver(ast.get()));
if (auto ok = validator.validate(*query); !ok) {
  // "column not found: nope (line 1:27)  [27,31)"
  fmt::print("{}\n{}\n", ok.error().to_string(),
             stmt::sspan_caret(sql, ok.error().span, ok.error().message,
                               sql::ansi::enabled_by_default()));
}
```

输出形如（值级定位：INSERT/UPDATE 的错误直接指向那个字面量）：

```
INSERT INTO users (id, age) VALUES (1, 'abc');
                                       ^^^^^ Type mismatch @ age (line 1:40-45)

SELECT * FROM users WHERE nope = 1;
                          ^^^^ column not found: nope (line 1:27-31)
```

- **定位优先级**：值自身的位置（`InsertQuery::value_spans` /
  `UpdateQuery::Assignment::value_span`）> 名字的位置（resolver）> 未知；
  这两处 span 是 builder 从 AST 值节点带过来的，**只用于诊断**，
  不参与比较/编码/键，构造 Query 的其它代码可以不填。
- `make_span_resolver(ast)` 会遍历 AST 收集"名字 → 位置"（表名/库名/列名，
  大小写不敏感）；**不传 resolver 时 span 为"未知"**，其余行为完全不变。
- `sspan_to_string(span)` 得到 `line 1:27-31`（跨行是 `line 2:3..4:5`）。
- 位置是"行/列（按字节）"，列区间左闭右开；多行 token（字符串、空行）也正确。
- 语法错误的位置仍由 parser 的 `line N` 给出（`ParseResult::error`）。

## 语法高亮

`statement/sql_highlight.h` 提供两个纯展示函数（`colors=false` 时原样返回）：

```cpp
stmt::highlight_sql(sql, colors);              // 整条语句着色
stmt::highlight_span(sql, span, colors);       // 只把 span 覆盖的片段标亮红
stmt::sspan_caret(sql, span, label, colors);   // 报错行 + caret，颜色如上
```

**实现原则：不自己扫描 SQL，复用 lexer 的 token 流。**

```
SQL 文本 ──► parser/sql.l（唯一的词法规则）
                │  lex_collect_tokens()  ← 与解析共用同一套规则
                ▼
        token 流 (kind, span, offset, length)
                │  statement/sql_highlight.cpp 只做 kind → 颜色
                ▼
        着色文本（token 之间的空隙原样复制，逐字节还原原文）
```

- 颜色表：`TOK_TYPE_NAME` 青、`TOK_STRING` 绿、`TOK_NUMBER` 黄、
  `TOK_NULL/TRUE/FALSE` 品红、其它 `TOK_*`（含 `IN/IS/LIKE` 与所有关键字）粗蓝、
  比较运算符粗体、`TOK_IDENT` 与单字符标点不着色、`TOK_LEX_ERROR` 标红。
  **这里没有关键字清单**：`sql.l` 里新增关键字只改 `sql.l`/`sql.y`，高亮自动跟随。
- 空白与注释不产生 token，由调用方用相邻 token 的 `offset` 之间的空隙还原，
  所以"去掉 ANSI 转义后 == 原文"是一条可测的不变量（已有用例）。
- 限制：`lex_collect_tokens()` 会自行建立/销毁扫描缓冲，**不能在 `yyparse()`
  进行中调用**；它与解析器一样是非重入的。

