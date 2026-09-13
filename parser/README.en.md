# parser module

A C-style lexer (Flex) + parser (Bison) that produces an AST.

中文版：[README.md](README.md)

## Scope and boundaries

```
SQL text --sql.l--> tokens --sql.y--> AST (C structs)
                                       |
                                       +--> statement module (C++): AST -> sql::ColumnDef / Value / Query
                                                |
                                                +--> sql_types: type semantics and range checks
```

- **The parser only has to parse.** It recognizes type names, aliases and the
  optional length and produces the AST. The *semantic* checks on lengths
  (legal? within bounds?) belong to `sql_types` / `statement`.
- The parser **does not depend on C++ or sql_types**: it only includes the C
  header `common/c_types.h`. The AST is `extern "C"` + POD so both C and C++
  can consume it.
- One exception: syntax-level length checks (`CHAR(300)`, `TEXT(10)`) fail at
  parse time, using the same constants as sql_types so the rules are not
  duplicated.

## Single source of truth for type definitions

Type names/aliases and length limits are defined exactly once, in
**`common/c_types.h`**:

| Item | Location | Consumers |
|------|----------|-----------|
| Type name/alias table | `c_type_aliases[]` | `sql.l` (identifier -> `TOK_TYPE_NAME`), `sql_types::string_to_data_type` |
| Length limits | `CTYPE_MAX_CHAR_LEN` / `CTYPE_MAX_VARCHAR_LEN` / `CTYPE_MAX_TEXT_LEN` | `sql.y` (length checks), `sql_types::kMax*Length` |
| Lookup/validation helpers | `c_type_lookup()` / `c_type_length_valid()` / `c_type_accepts_length()` | both of the above |

The only remaining duplication is the two `DataType <-> CDataType` switches
(the price of decoupling C and C++); the `TypeAliasTablesStayInSync` test in
`tests/test_parser/test_type_parsing.cpp` keeps both sides honest.

Adding a type means touching: the enum and alias table in
`common/c_types.h`, plus `sql_types::DataType` and `to_c/from_c`
(if it is a new family).

## Build

```bash
# bison >= 3.0 required (the system /usr/bin/bison is 2.3 and won't work)
cmake -S . -B build -DCMAKE_C_COMPILER=clang-mp-23 -DCMAKE_CXX_COMPILER=clang++-mp-23
cmake --build build -j4
ctest --test-dir build -R 'test_parser|test_sql_types' --output-on-failure
```

- The bison path defaults to `/usr/local/opt/bison/bin/bison` in `CMakeLists.txt`.
- Generated files (`parser.tab.c/h`, `lex.yy.c/h`) go to **`build/parser/`**;
  the source tree no longer keeps them. `parser.cpp` pulls the generated
  headers in through `extern "C" { ... }` so stale copies in the source tree
  can never be picked up by accident.
- The legacy `Makefile` used to write those generated files into `parser/`;
  it no longer builds the parser at all (the Makefile is now only a CMake
  wrapper).

## Support matrix

### Supported

| Category | Content |
|----------|---------|
| DDL | `CREATE/DROP DATABASE`, `CREATE/DROP TABLE`, inline `PRIMARY KEY`, `NOT NULL` / `NULL` |
| DML | `SELECT` (with `WHERE`/`ORDER BY`/`LIMIT`/`OFFSET`), `INSERT` (column list + **multi-row VALUES**: `VALUES (..),(..)`), `UPDATE`, `DELETE` |
| Transactions | `BEGIN [WORK\|TRANSACTION]`, `START TRANSACTION`, `COMMIT [WORK]`, `END [WORK]`, `ROLLBACK [WORK]`, `ABORT [WORK]` (aliases are normalized to `TXN_BEGIN/TXN_COMMIT/TXN_ROLLBACK` in the grammar actions) |
| Statement prefix | `EXPLAIN [ANALYZE] <statement>` -> `NODE_EXPLAIN` (wraps the explained statement; it is not a query, the session unwraps it) |
| Conditions | `= != <> > >= < <=`, `AND/OR/NOT`, `IN/NOT IN`, `LIKE/NOT LIKE`, `IS [NOT] NULL`, parentheses |
| Types | `TINYINT/INT8`, `SMALLINT/INT16`, `INT/INTEGER/INT32`, `BIGINT/INT64`, `CHAR(n)`, `VARCHAR(n)`/`VARYING`, `TEXT`, `BOOLEAN/BOOL`, `DATE`, `TIME`, `DATETIME/TIMESTAMP` |
| Literals | integers (int64, out-of-range is an error), negative numbers, `NULL`, `TRUE`/`FALSE`, strings (`''` escaping) |

### Not supported (syntax errors — never silently ignored)

| Item | Note |
|------|------|
| Table-level constraints | `PRIMARY KEY (a, b)`, `UNIQUE`, `CHECK`, `REFERENCES` |
| Column defaults | `DEFAULT ...`, `AUTO_INCREMENT` |
| `IF NOT EXISTS` / `IF EXISTS` | idempotent DDL |
| Quoted identifiers | `"col"`, `` `col` `` — use single quotes for strings |
| Backslash escapes in strings | `'\n'`, `\'` (standard SQL `''` is supported) |
| Savepoints & transaction modes | `SAVEPOINT`, `ROLLBACK TO SAVEPOINT`, `COMMIT AND CHAIN`, `START TRANSACTION ISOLATION LEVEL ...` (**explicit reason**, not a bare syntax error) |
| Others | JOIN, subqueries, `DISTINCT`, aggregates, `GROUP BY`/`HAVING`, `BETWEEN`, arithmetic expressions |

## Error handling conventions

- **Library code never prints.** Callers that want logs register a callback via
  `parser::Parser::set_log_callback()` (the CLI does when asked).
- Only the **first** error is kept: a specific lexer error (e.g.
  `integer literal out of range`) is not overwritten by the later
  `syntax error`.
- Line numbers come from flex's `yylineno` (`%option yylineno`); messages look
  like `line 4: syntax error, unexpected ...`.
- Unknown characters are no longer swallowed: the grammar reports them
  (`-5` parses as a negative number instead of `5`; `"t"` is a syntax error
  instead of being treated as `t`).

## Multithreading / server use (P0: thread-safety boundaries)

`parser::Parser::parse()` is **thread-safe** (serialized process-wide), but the
exact boundary matters:

| Global state | Who touches it | What we do now |
|---|---|---|
| flex scanner buffer, `yytext/yyleng/yylloc/yylineno` (`sql.l` is not reentrant) | every parse | one **process-wide mutex** in `parse()` wraps the whole parse |
| `g_parsed_ast` in `parser/ast.cpp` (written by the grammar action `set_parsed_ast`) | every parse | same lock |
| `g_parser_state` in `parser/parser.cpp` (the `yyerror` error sink) | every parse | same lock, and the sink is registered **only for the duration of the parse** (RAII clears `instance`), so no other `Parser` is left behind |
| `ast_debug` in `parser/ast.cpp` | written by the parsing thread, **read by other threads** in `free_ast` | now `std::atomic<int>` |
| `lex_collect_tokens()` (syntax highlighting / tools) | every call | **still not reentrant**: the server must not call it — return the span to the client and let the client (CLI) highlight |

- Cost: parsing is serialized. A statement parses in microseconds while the
  engine is "single writer + synchronous execution", so this is not the
  bottleneck; when parallel parsing is actually needed, follow the checklist
  below.
- **Reentrancy checklist (M3)**: bison `%define api.pure full` + `%parse-param`
  (pass the error sink and result pointer per parse) + `%lex-param`; flex
  `%option reentrant` (`yyscan_t` + `yylex_init_extra`); turn the
  `set_parsed_ast` global into `ctx->result`; give `lex_collect_tokens()` its
  own scanner per call; then delete the mutex.

Evidence: `tests/test_parser/test_thread_safety.cpp` (8 threads x 300
iterations x 2 cases). Before the lock it **always crashed**
(`fatal flex scanner internal error--end of buffer missed`); after the lock it
is stable, and ThreadSanitizer reports 0 races.

## Implementation notes

- Declarations of generated code inside `parser.cpp` must use C linkage
  (`extern "C"`), because `parser.tab.c` / `lex.yy.c` are compiled as C.
- `yyerror` is declared by `sql.y` and defined in `parser.cpp` (C linkage).
- `LIMIT/OFFSET` AST fields are `int`; the grammar range-checks them.
- `INSERT`'s `values` is uniformly a **list of rows** (each element is one
  row's `LIST_VALUE`); a single row is simply a list of length 1. That keeps
  `VALUES (1,2)` and `VALUES (1,2),(3,4)` structurally identical so the builder
  has one path (and the left-recursive rule keeps the grammar unambiguous).
- **Do not run clang-format on `sql.l` / `sql.y`**: they are not C/C++
  (`%{`, `%token`, `%type` get re-flowed and re-indented, and flex/bison then
  fail with `bad character: #`). Keep those two files hand-formatted;
  `ast.cpp` / `ast.h` are formatted normally.
- The AST node registry `nodes[]` is ordered by `NodeType` and has a
  compile-time length check; adding a node means updating `NodeType`, the
  struct and the registry together.
- Transaction keywords (`BEGIN/START/TRANSACTION/WORK/COMMIT/END/ROLLBACK/ABORT`)
  are **reserved words** — like `END`/`DESC` they can no longer be table or
  column names (in standard SQL `BEGIN/COMMIT/ROLLBACK/END/ABORT/START` are
  reserved anyway; `TRANSACTION`/`WORK` are extra). When keywords live in the
  upper layer as text matching, comments, casing and combinations like
  `EXPLAIN BEGIN` bypass the lexer/grammar — that is why they live here, and
  syntax highlighting follows automatically
  (`tests/test_parser/test_transaction.cpp`).
- `EXPLAIN`/`ANALYZE` are reserved words for the same reason. `EXPLAIN`'s AST
  is a **prefix node** `NODE_EXPLAIN{analyze, statement}`: it wraps the
  explained statement (whose node type is unchanged), so `EXPLAIN BEGIN` /
  `EXPLAIN SELCT` are located, highlighted and reported at the grammar level,
  while "should this run?" is decided by the session
  (`tests/test_parser/test_explain.cpp`).
