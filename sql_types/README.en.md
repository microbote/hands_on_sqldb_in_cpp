# sql_types — the SQL domain type module

中文版：[README.md](README.md)

This module owns **SQL domain values and types**: the type system, values,
rows, table schemas, condition trees, key ranges, key encoding, serialization,
three-valued logic and temporal types. It depends on no storage engine and
contains no Executor/Optimizer/Planner; it is the shared foundation of
`relation` / `statement` / `storage`.

> Structure, file list and invariants below mirror the Chinese version, which
> is the detailed source of truth. Code snippets are language-neutral.

## Files

| File | Contents |
|------|----------|
| `field_type.h` | `DataType` / `DataTypeClass`, families, promotion, integer/string capacities, `VARCHAR(n)` parsing, name conversion, C-API conversion |
| `value.h` / `value.cpp` | `Value` (variant storage), equality, text/temporal parsing, key codec entry points |
| `key.h` | `KeyCodecs`: Value <-> key encoding (the storage format contract), total order |
| `key_range.h` / `key_range.cpp` | `KeyRange`: ranges and set operations in key space |
| `key_set.h` | `KeySet`: point sets and set operations |
| `row.h` / `row.cpp` | `Row`, `RowBuilder`, row serialization |
| `schema.h` / `schema.cpp` | `ColumnDef`, `TableSchema`, `SchemaError`, schema serialization and validation |
| `condition.h` / `condition_types.h` / `condition_visitor.h` | condition tree and visitors |
| `compare_op.h` / `compare_op.cpp` | `CompareOp` and its traits |
| `sql_truth.h` | **three-valued logic**: `Truth`, truth tables, `sql_compare_op`, promoted comparisons, LIKE, WHERE evaluation |
| `temporal.h` / `temporal.cpp` | DATE / TIME / DATETIME parsing, formatting and calendar math |
| `query.h` / `query_clause.h` | the `Query` variant and SELECT/INSERT/UPDATE/DELETE/DDL payloads |
| `catalog.h` / `db_view.h` | pure logical metadata interfaces (Catalog / DatabaseView) |
| `identifier.h` | `Identifier`: keeps the original spelling, compares/hashes case-insensitively |
| `byte_buffer.h` | length-prefixed byte buffer reads/writes (used by serialization) |

## Core invariants

Covered by `tests/test_sql_types`; keep them when changing code.

### 1. Type system and promotion (`field_type.h`)

- Logical types: `TINYINT / SMALLINT / INT / BIGINT` (integers),
  `VARCHAR / TEXT`, `BOOLEAN`, `DATE / TIME / DATETIME`, `NULL`, `UNKNOWN`.
- **Integer width only exists in the schema/validation layer**: `Value` always
  stores `int64_t` and the encoding is identical; `can_represent()` performs
  range checks on write/read (300 into a `TINYINT` column -> `VALUE_OUT_OF_RANGE`).
- Families: integers, strings, temporals. `is_same_family()` decides whether a
  value may be written into a column; `common_type()` gives the type both sides
  are promoted to for comparison (`TINYINT+INT -> INT`,
  `DATE+DATETIME -> DATETIME`, `VARCHAR+TEXT -> TEXT`).
- Temporals are not implicitly converted: `DATE` (days) and `DATETIME`
  (seconds) have different units, so writing one into the other is
  `COLUMN_TYPE_MISMATCH`; comparisons promote to seconds.

### 1b. String capacities (`field_type.h` / `ColumnDef::length`)

Exactly isomorphic to "integer width lives in the schema layer":

| Type | Declared length | Capacity (bytes) |
|------|-----------------|------------------|
| `CHAR(n)` | 1..255 | n, 255 when undeclared |
| `VARCHAR(n)` | 1..65535 | n, 65535 when undeclared |
| `TEXT` | no length accepted | 65535 |

- The declared length lives in **`ColumnDef::length`** (0 = undeclared) and only
  takes part in schema validation. `Value` stores no length and **the key
  encoding has none** — so `CHAR/VARCHAR/TEXT` share the string family tag
  `0x02`, the same value produces the same key, and NULL keys match too (the
  same convention as `TINYINT..BIGINT` sharing `0x01`).
- Write checks: `can_represent_length()` compares **bytes** against the column
  capacity and returns `VALUE_OUT_OF_RANGE` when too long (equivalent to MySQL
  strict mode — no silent truncation). One UTF-8 CJK character is 3 bytes.
- Read checks: `Row::deserialize()` runs the same `validate_value()`, so legacy
  data that no longer fits a narrowed schema is rejected too.
- DDL: `string_to_data_type()` accepts plain names; use
  `parse_type_with_length("VARCHAR(32)", type, length)` for lengths. Illegal
  declarations (`CHAR(300)`, `VARCHAR(0)`, `TEXT(10)`, `INT(4)`) fail inside
  `TableSchema::add_column()` with `INVALID_COLUMN_DEF`.
- Promotion: `CHAR -> VARCHAR -> TEXT` (`common_type`); when a "result column
  length" must be derived, `common_string_capacity()` picks the larger capacity
  (the "sum" rule for CONCAT belongs to the SQL layer, not here).
- **Two things we do not do**: CHAR is not space-padded (MySQL's PAD SPACE
  collation makes `'abc' = 'abc '`; we compare binary and treat them as
  different), and comparisons are full bytewise — a declared length never
  truncates.

### 2. Value (`value.h`)

- Storage: `std::variant<std::monostate, int64_t, bool, std::string>`.
  Integers and temporals are `int64_t`, strings are `std::string` (SSO), NULL is
  `monostate`.
- `Value(5)` normalizes to `BIGINT` (the widest integer); to keep a width use
  `Value(5, DataType::TINYINT)` / `Value::tinyint(5)`.
- **Only `operator==`/`!=` remain** (container semantics: NULL equals NULL);
  `<`, `>` etc. were removed:
  - storage total order -> `KeyCodecs::compare()` / `less()` (NULL smallest);
  - SQL comparison -> `sql_compare_op()` in `sql_truth.h` (anything involving
    NULL is UNKNOWN).
- A moved-from value is "valid but unspecified" (the standard behaviour); it is
  no longer guaranteed to become NULL.

### 3. Key encoding format v3 (`key.h`)

```
key = [1-byte family tag][1-byte null flag][payload]
  null flag: 0x00 = NULL, 0x01 = value present
```

| tag | family | payload |
|-----|--------|---------|
| `0x01` | integers + DATE/TIME/DATETIME | `(uint64)v ^ (1<<63)`, big-endian, 8 bytes |
| `0x02` | CHAR / VARCHAR / TEXT | escaped bytes + `0x00` (`0x00` -> `0x00 0xFF`) |
| `0x03` | BOOLEAN | `0x00` / `0x01` |
| `0x00` | NULL without column type information | none (flag byte only) |

- Strings use escaping plus a terminator so that **key byte order ==
  lexicographic order** (an early length-prefix version made `"b" < "aaaa"`
  and lost rows in range scans).
- **NULL encoding depends on the column type**: storage uses
  `Value::to_key(column_type)` (or `KeyCodecs::to_key(v, type)`), encoding NULL
  as `[family tag] 0x00` — the smallest key of that family. The logical layer
  (KeySet ordering etc.) has no column type and uses `Value::to_key()`, where
  NULL becomes `[0x00] 0x00` and sorts before everything.
- Family bounds: `min_key_for_type(type)` = `[tag] 0x00` (including NULL),
  `upper_key_for_type(type)` = `[tag+1]`. A family scan `[min, upper)`
  therefore covers "this family's NULL plus all its values" and never leaks
  into another family.
- `inclusive_upper_bound(k) = k + 0x00` expresses point/closed upper bounds.
- Decoding errors return NULL (no exceptions); decoding preserves the logical
  type the caller asked for (DATE stays DATE, TEXT stays TEXT).

### 4. Three-valued logic (`sql_truth.h`)

`Truth ∈ {TRUE, FALSE, UNKNOWN}`, where NULL means UNKNOWN:

| Expression | Result |
|------------|--------|
| `NULL = 5` / `NULL < 5` | UNKNOWN |
| `NULL = NULL` / `NULL <> NULL` | UNKNOWN (not TRUE!) |
| `NULL IS NULL` | TRUE |
| `5 IS NULL` | FALSE |
| `FALSE AND UNKNOWN` | FALSE |
| `TRUE OR UNKNOWN` | TRUE |
| `NOT UNKNOWN` | UNKNOWN |

- `WHERE` keeps only TRUE rows (`where_keeps()`): FALSE and UNKNOWN are both
  filtered out, so rows with `NULL <> 5` never appear.
- `x IN (a, b)` ≡ `x = a OR x = b`; with a NULL in the list and no match the
  result is UNKNOWN (`NOT IN` likewise never returns TRUE by accident).
- `sql_equal_null_safe()` is used for DISTINCT / GROUP BY / JOIN keys: NULL and
  NULL count as one group.
- Cross-family comparison performs only two MySQL-style coercions: number vs
  string (parse the string as an integer) and temporal vs string (parse by that
  temporal type). **A failed parse yields UNKNOWN**, which is stricter than
  MySQL's truncate-to-0-and-warn (it avoids `id = 'abc'` matching by accident).
- Condition evaluation: `evaluate_condition(cond, lookup)` (`lookup` returning
  nullptr means the column does not exist -> UNKNOWN).
- LIKE: `%` any run, `_` one character, `\` escapes. **Currently a
  case-sensitive binary comparison**; collations are not implemented.

### 5. Temporal types (`temporal.h`)

| Type | Storage | Text form |
|------|---------|-----------|
| DATE | days since 1970-01-01 | `YYYY-MM-DD` |
| TIME | seconds since 00:00:00 | `HH:MM[:SS]` |
| DATETIME | Unix seconds (UTC, no timezone) | `YYYY-MM-DD[ T]HH:MM[:SS]` |

Ranges: DATE covers 0001-01-01..9999-12-31, TIME is [0, 86399]. Parsing is
strict (leap years, month/day/hour/minute/second bounds) and returns `nullopt`
on failure.

### 6. KeyRange semantics (`key_range.h`)

- Default `[low, high)`; `low_ == nullopt` is -inf, `high_ == nullopt` is +inf;
  a NULL bound counts as unbounded.
- Two direction flags, `low_exclusive_` / `high_inclusive_`, express `(a, b]`,
  `[a, b]` and `{a}`.
- Every predicate (contains / overlaps / covers / intersect / unite / subtract /
  complement) works on encoded key bytes, guaranteeing that `contains()` agrees
  exactly with "scan what `to_str_key_range()` returns" — which is why string
  points and closed ranges work.
- Set operations propagate the known type (`type()`).

#### `StrKeyRange` returned by `to_str_key_range()` (physical scans)

The physical layer understands exactly **one** form: half-open intervals. All
logical semantics (NULL / open-closed / inclusivity) are **flattened into the
key** by `to_str_key_range()`, so a scanner only needs `Seek(start)` plus
`key() < end`.

```cpp
struct StrKeyRange {
  Key start, end;                                  // concrete keys
  bool is_empty() const { return !(start < end); }  // start >= end means empty
};
```

Flattening rules:

| Logical (KeyRange) | Physical (StrKeyRange) |
|---|---|
| unbounded lower (-inf) or "include NULL" | `start = [tag][0x00]` (NULL's key, also the family minimum) |
| lower bound is value v (inclusive) | `start = key(v)` |
| lower bound is value v (exclusive) | `start = key(v) + 0x00` |
| "exclude NULL" lower bound | `start = [tag][0x00] + 0x00` (equivalent to `[tag][0x01]`) |
| unbounded upper (+inf) | `end = [tag+1]` (just past every key of the family; exclusive by nature) |
| upper bound is value v (exclusive) | `end = key(v)` |
| upper bound is value v (inclusive) | `end = key(v) + 0x00` |
| empty set | `start == end` |

- **Why no flags**: the scan layer should stay as dumb as possible. Open/closed
  is decided in the logical layer and flattened to one form, so there is no
  "consumer forgot a flag -> silently skipped a row" failure mode (the main
  failure mode of the flags design).
- **`+0x00` is a universal successor**: appending 0x00 always lands immediately
  after — it cannot overflow, leak into another family, be decoded as a valid
  value (decoding requires a terminating 0x00 at the end), or collide with any
  value's key. So `<= type max`, `> x` and `(a, b]` are all expressible, while
  `[tag+1]` still serves as +inf.
- Tag space: family tags are in {0x00,0x01,0x02,0x03} (always < 0x0F) and
  **keys without a value start with 0xFF**, so `[0xFF]` is a safe "global +inf"
  (also `upper_key_for_type(UNKNOWN)`) and can serve as the upper bound of
  "everything under a prefix".
- `start`/`end` are **scan bounds, not storable keys** (`is_valid_key()`
  returns false for `key+0x00` and for family upper bounds).

#### KeyRange completeness (NULL / ±inf / open-closed / set operations)

| Dimension | Representation | Note |
|---|---|---|
| -inf | `low_ == nullopt` | flattened to the family minimum (NULL's key) |
| +inf | `high_ == nullopt` | flattened to `[tag+1]` (exclusive) |
| includes NULL | no lower bound, or `low_ = NULL` with `low_exclusive_ = false` | equivalent; construction/operations `normalize()` to "no lower bound" |
| excludes NULL | `low_ = NULL` with `low_exclusive_ = true` | flattened to `NULL key + 0x00`; a concrete lower bound excludes NULL naturally |
| NULL only | `high_ = NULL` with `high_inclusive_ = true` | a point; `to_string()` prints `{NULL}` |
| upper bound NULL, exclusive | — | treated as **empty** (nothing sorts before NULL) |
| open/closed | `low_exclusive_` / `high_inclusive_` | all four combinations, including `(a, b]` (produced by `subtract`) |
| point | `[v, v]` | no longer `[v, v+1)`, avoiding +1 overflow at `INT64_MAX` |
| empty | `is_empty()` | includes "NULL exclusive upper" and "low > high" |

Predicate factories (used by the optimizer; NULLs are already excluded per the
three-valued logic):

| Predicate | Factory | Range |
|-----------|---------|-------|
| `id > 5` | `gt(Value(5))` | `(5, +inf)`, no NULL |
| `id >= 5` | `ge(Value(5))` | `[5, +inf)`, no NULL |
| `id < 5` | `lt(Value(5))` | `(-inf, 5)`, no NULL (unlike bare `to(5)`) |
| `id <= 5` | `le(Value(5))` | `(-inf, 5]`, no NULL |
| `id = 5` | `eq(Value(5))` | point |
| `id IS NULL` | `null_only(type)` | NULL only |
| `id IS NOT NULL` | `non_null(type)` | values only |
| no predicate | `all(type)` | includes NULL |
| argument is NULL | the factories above | empty set (a comparison can never hold); `eq(NULL, type)` = `IS NULL` |

Set operations (`intersect` / `unite` / `subtract` / `complement`) all work on
encoded keys, so NULL semantics follow automatically and these identities hold
(tested):

```
all ∩ non_null = non_null          complement(non_null)  = null_only
null_only ∪ non_null = all         complement(null_only) = non_null
all − non_null = null_only         non_null − null_only  = non_null
```

### 7. Serialization format v1 (`row.cpp` / `schema.cpp`)

Both use **length-prefixed framing** from `byte_buffer.h` instead of `|`, `,`
or `:` separators (key encodings are binary and identifiers may contain those
characters, so a separator will eventually be broken by content).

```
Row:    [u8 version][u32 field_count]{ [str key] }*
Schema: [u8 version][str table_name][u32 pk_slot][u32 column_count]
        { [str name][u8 type][u8 flags] }*      # pk_slot 0 = no primary key
```

- `Row::serialize(schema)` encodes every field with its column type
  (recommended); `Row::serialize()` has no schema and uses the typeless NULL
  key for NULLs.
- Deserialization returns `std::expected`: `INVALID_FORMAT` (corrupt or wrong
  version), `COLUMN_SIZE_MISMATCH` (field count differs from the schema),
  `VALUE_OUT_OF_RANGE` (a stored value no longer fits its column).

### 8. Error handling conventions

- Expected failures (parsing, validation, deserialization, building rows) ->
  `std::expected` with an error code.
- Programming errors (wrong type, out-of-range index) -> exceptions:
  `Value::as_int()` throws `std::runtime_error`, `TableSchema::column_at()`
  throws `std::out_of_range`, text parsing failures throw
  `std::invalid_argument` / `std::out_of_range`.
- **Library code never logs**; callers decide how to report.

## Build and test

```bash
cmake --build build -j4                 # build sql_types and its tests
./build/run_tests/test_sql_types        # run them
make sql-types-test                     # equivalent one-liner
```

## Not done / boundaries

- String comparison and LIKE use a binary collation (case-sensitive); there is
  no `utf8mb4_*_ci`-style collation.
- No FLOAT/DOUBLE/DECIMAL; DATETIME has no timezone and no `TIMESTAMP`
  auto-update semantics.
- Strings are CHAR / VARCHAR / TEXT only: no MEDIUMTEXT / LONGTEXT / BLOB /
  VARBINARY, no character sets or collations (lengths are bytes, not
  characters).
- CHAR has no space-padding semantics (validated by capacity, stored without
  padding).
- Temporals are not implicitly converted (DATE != DATETIME on write); the SQL
  layer must convert explicitly.
- Cross-family comparison of a non-numeric string with a number yields UNKNOWN
  (MySQL truncates to 0 and warns).
- Single-column primary keys only; composite keys and `KeyPrefix` are out of
  scope here.
- `KeySet::add()` deduplicates linearly, so bulk insertion is O(n²).
