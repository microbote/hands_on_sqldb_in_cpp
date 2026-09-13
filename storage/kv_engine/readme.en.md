# KV layer: transactions (pessimistic single writer, buffer-then-commit)

中文版：[readme.md](readme.md)

## 1. Why this design

LevelDB provides the two building blocks we need:

| Block | What it gives us |
|---|---|
| `WriteBatch` | **Atomicity**: written as one WAL record, replayed as a whole on recovery — all or nothing |
| `WriteOptions{sync=true}` | **Durability**: fsync the WAL before committing |
| `GetSnapshot()` (not used yet) | Consistent reads; it is where repeatable-read transactions would hang |

So a transaction is:

```
during the transaction: writes go to TxBuffer (memory), the DB is untouched
commit                : TxBuffer -> one WriteBatch with sync=true, single write
rollback              : drop the TxBuffer
```

**"After a rollback the state is unchanged" holds by construction** — the DB was
never touched. That is an order of magnitude simpler than write-then-undo: no
private WAL, no crash recovery, no need to prove the undo is complete. Put
differently, the textbook "apply to a copy and swap a pointer atomically" is
just **one WriteBatch** on this storage.

## 2. Components

| File | Responsibility |
|---|---|
| `KVStore` in `kv_engine.h` | **the store** (one per process): data + locks + **the write slot**; `connect()` opens a connection |
| `KVEngine` in `kv_engine.h` | **one connection** (= one session's view of storage): its own transaction buffer; many connections share one store |
| `tx_buffer.h/.cpp` | `TxBuffer`: ordered op log (commit order) + per-key overlay view (for reads) |
| `OverlayCursor` in `tx_buffer.h` | ordered cursor over the overlay (used by the merge iterator; only walks keys this transaction touched) |
| `merging_iterator.h/.cpp` | `MergingIterator`: merges the DB iterator with the overlay cursor (newly inserted keys are merged in order) |
| `kv_engine.h` | `begin/commit/rollback_transaction`, `WriteBatch::remove_range`, `WriteBatch::sync` |
| `mock_engine` / `leveldb_engine` | two implementations of the same interface — **their semantics must match** |

### Store / connection, two layers (multiple connections)

```
kv::KVStore   one per process: data_ (mock) / db_ (leveldb) + lock + write slot
   └─ connect() -> kv::KVEngine  one connection = one session's view (own TxBuffer)
        ├─ connect() again (two connections share data and write slot)
        └─ ...
```

Usage (this is exactly how the CLI builds it; on a server each client calls
`connect()` once):

```cpp
auto store = kv::open_store(kv::EngineType::LEVELDB, options);  // open the store
auto conn  = store->connect();                                  // one connection
auto conn2 = store->connect();                                  // a second connection
```

Why the split is mandatory: LevelDB's directory lock does **not** stop a second
`DB::Open` in the *same process* (POSIX record locks are per process), and two
`leveldb::DB` objects writing the same files will corrupt data. "One process =
one `db_` handle + N connections" is therefore the only correct shape.

**The write slot lives on the store**: `begin_transaction()` asks the store and
only one connection may hold it at a time (the second gets `Status::Busy`).
`put/remove/write_batch` without an explicit transaction are "autocommit
writes": they briefly take the write slot, so the single-writer rule also holds
for engine-level calls (session write statements are wrapped in a transaction
anyway).

**What readers see** (consistency without snapshots):

- uncommitted writes live only in the writer's own `TxBuffer` -> other
  connections cannot see them (no dirty reads);
- a commit is one `WriteBatch` -> a reader sees before or after, never a
  half-applied state;
- **a single scan is internally consistent**: a LevelDB iterator pins the
  sequence at creation time, and the Mock iterator **materializes** its range
  into a vector at creation (no lock held, unaffected by later commits).
  Both engines behave identically — the property pinned by
  `Connections.ScanIsNotAffectedByAnotherConnectionsCommit`.

### Why TxBuffer keeps two representations

- `log_` (ordered op list): replayed in order on commit. The interleaving of
  `remove_range` and `put` changes the result, so "last state per key" is not
  enough.
- `view_` (per-key overlay): point reads must answer "what is this key now?",
  which merges repeated writes to one key. Range deletes participate via a
  sequence number: **the operation with the larger sequence wins**
  (`put` then `remove_range` -> deleted; `remove_range` then `put` -> written).

### The overlay answers with three states (no `optional<optional>`)

```cpp
struct OverlayOp {
  enum class Kind { kNone, kValue, kTombstone };
  Kind kind = Kind::kNone;
  ByteValue value;   // valid only for kValue
};
```

- `kNone` -> the key is answered by the layer below (the DB);
- `kValue` -> this transaction wrote a new value;
- `kTombstone` -> this transaction deleted it (including via `remove_range`).

Point reads and scans (`MergingIterator`) use the **same** `OverlayOp`
decision, so a scanned row and a point-read row can never disagree.

### Why scans must merge rather than filter

Autocommit only wraps the write statement itself, where filtering suffices. But
inside an explicit transaction, `BEGIN; INSERT; SELECT; COMMIT` must see the
freshly inserted rows — those keys **do not exist in the DB at all**, so the
overlay cursor has to insert them into the result stream in order. That is why
`MergingIterator` merges:

| Case | Behaviour |
|---|---|
| in the overlay, not in the DB (new insert) | inserted into the stream in order |
| in both (modified) | the overlay's new value wins |
| overlay tombstone (deleted) | consumed on both sides |
| only in the DB | passed through |

Range deletes (`remove_range`) are not expanded: DB keys are tested with
`lookup()`, and the overlay's own keys are ordered by sequence number.

## 3. Semantics and boundaries

| Item | Our choice |
|---|---|
| Concurrency | **multiple connections + pessimistic single writer**: one store per process, N connections (= N sessions); only one write transaction at a time, a second connection's `begin` returns `Status::Busy` |
| Isolation | no dirty reads (uncommitted data is not in the DB); commits become visible atomically; with a single writer write-write conflicts cannot happen |
| Readers | **never block, never wait for locks**: they read the committed state; one scan is internally consistent (see above) |
| Repeatable read | a write transaction is trivially repeatable (single writer: nobody else can commit during it). A future **read-only** repeatable-read transaction would use either "hold a shared read lock until the end of the transaction" or a `leveldb::Snapshot` — see section 7 |
| Conflict detection | **none** (not needed with a single writer). Multiple writers would need "read set + validation at commit" |
| Missing keys | `get`/`exists`/`remove` behave **identically on both engines**: `get` -> `NotFound`, `remove` of a missing key -> `NotFound` (leveldb's `Delete` returns OK natively, so `LevelDBStore::remove` looks first). Deletes **inside a transaction** are idempotent (they are buffered ops); deletes outside are immediate |
| Failed commit | the buffer is **kept** so the caller can retry or roll back (same on LevelDB and Mock) |
| Transaction size cap | `TxBuffer::kDefaultLimit` = 64MB; beyond that `commit` returns `InvalidArgument` |
| Explicit transactions | `BEGIN / COMMIT / ROLLBACK` (aliases `START TRANSACTION` / `END` / `ABORT`): **grammar-level keywords** (`transaction_stmt` in `parser/sql.y`), with execution and session state in the session (the autocommit guard yields inside an explicit transaction) |
| Statement failure inside a transaction | the transaction is marked aborted: later statements fail and only `ROLLBACK` is accepted; `COMMIT` in that state actually rolls back and reports it (Postgres style) |
| Validation-time errors | do **not** abort the transaction (nothing was written yet — MySQL style) |

## 4. Relationship with the layers above

- **session**: every write statement / DDL is wrapped by an `AutoCommit` guard
  (begin on construction, commit on success, automatic rollback when returning
  early) => **statement atomicity**: a failure halfway leaves no partial write,
  and DDL cannot leave a half-created table. Inside an explicit transaction the
  guard yields (it neither opens nor commits) and `COMMIT/ROLLBACK` finishes
  the job. **Read statements never open a transaction.**
- **relation**: `Table::truncate` and `KVCatalog::remove_prefix` use
  `WriteBatch::remove_range` (one op in the buffer instead of collecting keys
  first).

## 5. Tests

`tests/test_storage` (19 cases, storage layer; every case runs on both engines)
plus `tests/test_tx` (23 cases):

- buffered writes are invisible to the DB (`size()` bypasses the buffer) and
  visible after commit;
- rollback leaves everything as it was, and a new transaction can start;
- read-your-own-writes inside a transaction (put -> remove -> put ordering);
- forward and reverse scans both apply the overlay;
- only one write transaction at a time (`Busy`); empty commit/rollback return
  `NotFound`; **failed commit** (fault injection) leaves nothing behind and
  keeps the buffer for retry;
- `remove_range` ordering (`put` then delete vs delete then `put`);
- one invalid op rejects the whole batch (matching LevelDB's WriteBatch
  atomicity);
- the **same semantics are re-run on the LevelDB engine** (a hard lesson:
  LevelDB once deadlocked by re-locking during commit while only Mock was
  tested);
- the merge iterator: newly inserted keys show up in forward/reverse scans,
  overwritten keys use the new value, deleted keys disappear, and
  `seek`/`seek_to_last`/`prev` also see the overlay;
- **`Connections`** (5 cases, both engines): per-connection transaction
  buffers, commits visible across connections, a second writer getting `Busy`
  while readers keep reading, scans unaffected by another connection's commit,
  forward/reverse scans matching on both engines, and a connection dying with
  an open transaction = rollback + write slot released.

## 6. Pitfalls hit along the way (all covered by tests)

1. `TxIterator::normalize()` used the wrong direction: `Iterator::next()` already
   means "advance in scan direction" (reverse iterators call `--` internally);
   calling `prev()` by direction made reverse scans return nothing.
2. LevelDB commit deadlock: `commit_transaction()` held the lock and then called
   `write_batch()` (non-recursive mutex, same lock). Both engines now have
   `apply_batch_locked()` ("caller must hold the lock").
3. **`OverlayCursor` stored pointers to the scan range**: callers pass temporary
   `KeyRange` objects (e.g. `Table::scan`), so after the first row the pointer
   dangled — the symptom was "SELECT inside a transaction sees only the first
   row". Bounds are now copied into the cursor.
4. `prev()`/`seek_to_last()` on a merged stream cannot simply "step back once":
   between two sources you must pick the one that is *later* in scan direction.
5. `MockIterator` held a raw pointer to the map with no lock: concurrent writes
   during a scan were a data race. It now materializes its range.

## 7. Next steps

1. Multiple writers would add "read set + validation at commit" (global
   sequence -> precise read set -> per-key versions).
2. **Write-transaction working set (row locks): deliberately not doing this
   now** (decided 2026-09-13), for two reasons:
   - with a **single writer**, the write slot already gives stronger mutual
     exclusion, and row locks add no observable semantics for writers or
     readers (readers never see uncommitted rows anyway);
   - the real purpose of row locks is narrowing the lock range to support
     **multiple writers**, and that introduces **phantoms**: the premises of a
     transaction can change underneath it (e.g. the row count changes while the
     transaction reasons about it). That is hard to solve without **snapshot
     reads**, so row locks must land together with "multiple writers + snapshot
     reads", never half of it.
3. Read-only repeatable read: pick one — "read transaction holds a shared read
   lock until the end" (no MVCC, but commits wait for readers) or
   `leveldb::Snapshot` (readers never block commits, at the cost of version
   management and snapshot lifetime).
4. Large transactions (over 64MB) spilling to disk; this LevelDB build has no
   `DeleteRange`, so range deletes are expanded per key at commit time
   (atomicity unchanged, memory proportional to the key count).
