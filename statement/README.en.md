# statement module

Turns the AST produced by the parser into **sql_types' `sql::Query`** and does
semantic validation before execution.

中文版：[README.md](README.md)

```
SQL text --parser--> AST --StatementBuilder--> sql::Query --StatementValidator--> OK / StmtError
                                                              (queries sql::Catalog)
```

## Responsibilities of the two classes

| Class | Input | Output | Does | Does not |
|-------|-------|--------|------|-----------|
| `StatementBuilder` | `const ASTNode*` | `std::expected<sql::Query, StmtError>` | node-to-node conversion, structural checks (fields present, value nodes legal), SQL normalization (e.g. PRIMARY KEY => NOT NULL) | does not touch the Catalog or storage, **does not take ownership of the AST and does not modify it** (the interface takes `const ASTNode*`; lifetime is the caller's) |
| `StatementValidator` | `const sql::Query&` + `const sql::Catalog&` | `std::expected<void, StmtError>` | database/table/column existence, value families and ranges, primary-key constraints, DDL semantics (duplicate create, dropping what does not exist) | does not modify data; **passing validation != executed** |

Helper: `statement/source_span.h` (span formatting, caret rendering, name ->
position resolution).

### INSERT primary-key conflicts: checked before execution

Beyond the per-column checks, `StatementValidator::validate_insert()` calls
`check_primary_key_conflicts()` and detects two kinds of conflict:

1. **Duplicates inside this batch of VALUES** — a purely static check
   (deduplicate on the encoded `sql::Key`, i.e. `Value::to_key(pk_column_type)`,
   *exactly* how storage encodes keys). Any Catalog can do this.
2. **Conflicts with rows already in the table** — needs the Catalog to support
   data probing: `sql::Catalog::primary_key_exists(db, table, key)`
   (the default returns `nullopt` = unsupported; `KVCatalog` implements it by
   opening the table and doing a point lookup). When probing is unsupported we
   **degrade** to the execution-time check (`Table::insert` already reports
   duplicate keys).

The error code is `StmtErrorCode::DUPLICATE_PRIMARY_KEY`, and the span points
at **the offending value** (`value_spans`), so in a multi-row INSERT you can
see which row it was.

Why bother going earlier: in the session a write statement is "one statement,
one transaction", so a conflict found halfway through means a rollback;
rejecting it during validation means **the transaction is never opened**. In an
explicit transaction it matters even more — validation errors **do not abort**
the transaction (the statement/session contract), so the user can keep working
instead of being forced to `ROLLBACK`.

Transaction statements (`BEGIN` / `COMMIT` / `ROLLBACK`, aliases normalized in
the grammar) are a **pure structural conversion**: `NODE_TRANSACTION` ->
`sql::TransactionStmt{kind}`, which the validator passes straight through —
"am I already in a transaction / can I start one" is **session state** and
belongs to the session, not to builder/validator (see the `execute()` dispatch
in `session/session.cpp`).

`NODE_EXPLAIN` (`EXPLAIN [ANALYZE] <statement>`) **is not a query**, so the
builder rejects it: the caller (session) must unwrap it in `prepare()` and hand
the inner statement to the builder; passing it in directly yields
`UNSUPPORTED_AST_NODE` instead of being executed as an ordinary statement.

The caller (executor / shell) is responsible for applying a validated DDL to
the Catalog, or going through the upper transaction layer — the validator is
read-only.

Neither class **stores error state**: codes and messages travel with the return
value, so the same builder/validator can be reused and shared across threads,
and an old error can never stick to the next call (an early version kept a
member `error_` and had exactly that bug).

## Error handling (`stmt_defs.h`)

```cpp
enum class StmtErrorCode : uint8_t {
  OK = 0,
  // validation 1..49
  UNKNOWN_STMT_TYPE, VALIDATOR_NOT_INIT, DATABASE_NOT_FOUND,
  DATABASE_ALREADY_EXISTS, TABLE_NOT_FOUND, TABLE_ALREADY_EXISTS,
  COLUMN_NOT_FOUND, COLUMN_COUNT_MISMATCH, COLUMN_TYPE_MISMATCH,
  VALUE_OUT_OF_RANGE, COLUMN_ATTR_NULL_MISMATCH, DUPLICATE_COLUMN,
  DUPLICATE_PRIMARY_KEY, NO_PRIMARY_KEY, INVALID_SCHEMA, EMPTY_STATEMENT,
  // builder 99..
  AST_IS_NULL = 99, UNKNOWN_AST_Type, INVALID_AST_NODE, UNSUPPORTED_AST_NODE,
};

struct StmtError {              // errors are values: a code plus context
  StmtErrorCode code = StmtErrorCode::OK;
  std::string message;
  SSpan span;                   // from the AST; unknown means sspan_valid() == 0
  bool ok() const;
  explicit operator bool() const;
  std::string to_string() const;   // falls back to stmt_error_message(code)
};
```

Usage:

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

## Error localization (source spans)

An error is not just "this statement is wrong" — it can say **which name or
literal** is wrong:

```
sql.l (%option yylineno + its own column counter)
   │  yylloc{first_line, first_column, last_line, last_column}
   ▼
sql.y (%locations; AST_SET_SPAN($$, @$) / @n)
   │  ASTNode::span, SelectNode::table_span, DatabaseNode::db_span ...
   ▼
StmtError.span (the builder takes node positions; the validator resolves names)
   │
   ▼
sspan_caret(sql, span) -> rendered by the CLI
```

```cpp
stmt::StatementValidator validator(catalog, stmt::make_span_resolver(ast.get()));
if (auto ok = validator.validate(*query); !ok) {
  // "column not found: nope (line 1:27)  [27,31)"
  fmt::print("{}\n{}\n", ok.error().to_string(),
             stmt::sspan_caret(sql, ok.error().span, ok.error().message,
                               sql::ansi::enabled_by_default()));
}
```

```
INSERT INTO users (id, age) VALUES (1, 'abc');
                                       ^^^^^ Type mismatch @ age (line 1:40-45)
SELECT * FROM users WHERE nope = 1;
                          ^^^^ column not found: nope (line 1:27-31)
```

- **Priority**: the value's own span (`InsertQuery::value_spans` /
  `UpdateQuery::Assignment::value_span`) > the name's span (resolver) > unknown.
  Those spans are carried from AST value nodes and are **diagnostic only** —
  they never take part in comparison, encoding or keys, so code that builds
  Queries directly may leave them empty.
- `make_span_resolver(ast)` walks the AST to collect name -> span (tables,
  databases, columns; case-insensitive). **Without a resolver spans are
  "unknown"** and everything else behaves identically.
- `sspan_to_string(span)` yields `line 1:27-31` (multi-line:
  `line 2:3..4:5`).
- Columns are byte offsets, the column range is half-open; multi-line tokens
  (strings, blank lines) are handled correctly.
- Syntax errors still come from the parser as `line N` (`ParseResult::error`).

## Syntax highlighting

`statement/sql_highlight.h` exposes three presentation-only functions (with
`colors=false` they return the input unchanged):

```cpp
stmt::highlight_sql(sql, colors);              // color the whole statement
stmt::highlight_span(sql, span, colors);       // mark the span in red
stmt::sspan_caret(sql, span, label, colors);   // error line + caret
```

**Principle: never scan SQL again — reuse the lexer's token stream.**

```
SQL text ──► parser/sql.l (the only lexical rules)
                │  lex_collect_tokens()  <- same rules as parsing
                ▼
        token stream (kind, span, offset, length)
                │  statement/sql_highlight.cpp: kind -> color only
                ▼
        colored text (gaps between tokens copied verbatim, byte-exact)
```

- Palette: `TOK_TYPE_NAME` cyan, `TOK_STRING` green, `TOK_NUMBER` yellow,
  `TOK_NULL/TRUE/FALSE` magenta, other `TOK_*` (including `IN/IS/LIKE` and all
  keywords) bold blue, comparison operators bold, `TOK_IDENT` and single-char
  punctuation uncolored, `TOK_LEX_ERROR` red.
  **There is no keyword list here**: adding a keyword in `sql.l` is enough, the
  highlighting follows (that drift was a real bug before).
- Whitespace and comments produce no tokens; callers reconstruct them from the
  gaps between adjacent token offsets, so "strip ANSI == original" is a tested
  invariant.
- Limitation: `lex_collect_tokens()` builds and destroys its own scan buffer
  and must **not** be called while `yyparse()` is running; like the parser it
  is non-reentrant.
