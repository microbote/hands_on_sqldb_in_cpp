# raft module design: Multi-Raft for sqldb

中文版：[DESIGN.md](DESIGN.md)

**Status: design frozen; the P0 core is implemented (election / log
replication / commit / apply / LevelDB log persistence / proposal payload
codec / KV state-machine bridge).** This is an implementation plan, not a
user guide; responsibilities / interfaces / pitfalls live in `raft/README.md`.

## 0. Goal and scope

Add **multi-raft** to sqldb: split the data into several raft groups by key
range, each electing its own leader and replicating independently, **reusing
the existing network/coroutine framework** and leaving the SQL layer untouched.

Two decisions are already made, and everything below follows from them:

| # | Decision | Consequence |
|---|----------|-------------|
| **A** | **Replicate the KV `WriteBatch` directly** (not SQL, not conditional commands) | the state machine is the KV engine; planner/executor stay out of apply |
| **B** | **Cross-group write transactions are forbidden**; **cross-group read-only** is decided by the client mode `strict` / `loose` (§3.3) | write transactions are pinned to one group; write concurrency = today's "pessimistic single writer", narrowed to one group |

**Out of scope** (explicitly not doing, to avoid scope creep): cross-group
**write** transactions and 2PC, a global database-wide lock, **globally
consistent snapshots (global timestamp / MVCC)**, intra-table split/merge,
membership changes (joint consensus), follower read / lease, geo-replication.

## 1. Architecture

```
client ──SQL protocol──► sqldb-server (svrkit::TcpServer)     ← reused, unchanged
                            │ Session / statement / planner /
                            │ executor / relation              ← all unchanged
                            ▼
                      kv::KVStore / kv::KVEngine               ← same interface, new impl
                ┌───────────┴────────────┐
         RaftKVStore (new)         local LevelDB (as-is, the state machine)
                │ MultiRaft
                ├─ RaftNode × N (one per group)
                └─ raft transport: svrkit::TcpServer + common/net (separate port)
```

### 1.1 Why the seam is `kv::KVStore` / `kv::KVEngine`

1. `Session → statement → relation → kv` is already a clean interface; swapping
   the implementation needs no change above it;
2. `WriteBatch` is the natural replication unit — a set of concrete `put` /
   `remove` / `remove_range` ops (decision A makes it the log entry payload);
3. **the key layout is already shard-ready**: `@data/<len db>/<len table>/` is a
   contiguous prefix and `prefix_end()` in `relation/key_prefix.h` is its right
   bound — so "**one table = one group**" aligns for free, with no new notion of
   a shard boundary;
4. `KVStore` already has the shape "one per process + `connect()` per session",
   which maps directly onto "one local state machine + many connections".

## 2. Sharding model: key → group

### 2.1 Granularity: **one table = one raft group**

| key prefix | owner |
|------------|-------|
| `@data/<len db>/<len table>/<encoded pk>` | the group holding that **table** |
| `@system/*` (`@system/databases`, `@system/tables/<db>`, `@system/schema/<db>/<table>`, `@system/tablestats/...`, `@system/dbstats/...`) | **group 0 (the meta group)**, always |

Why:

- `@data/<db>/<table>/` is a contiguous prefix, so
  `[prefix, prefix_end(prefix))` is an exact range — no need to invent a
  boundary;
- `DROP TABLE` / `TRUNCATE` go through `WriteBatch::remove_range`, and a bulk
  range delete **must land in a single group**; one-table-one-group satisfies
  this by construction;
- splitting a table further would make `remove_range`, full scans and table
  statistics cross groups at the same time. Cost far exceeds benefit; not doing
  it.

### 2.2 Routing table

`key → group` is an **ordered range table**: `[start_key, end_key) → group_id`.

- **P1**: static config (`shard.N = <start>,<end>`), a single group at first;
- **P2**: generated per table, one range each;
- **P3**: managed by the `_meta` group (which is also the precondition for
  split/merge).

All `@system/*` keys go to group 0. **This must be hard-wired at the front of
the routing table**: forget it and you get the bizarre symptom "`CREATE
DATABASE` only took effect on the leader; other nodes cannot see the database".

## 3. Transaction rules (the concrete shape of decision B)

### 3.1 Rules

1. **A write transaction** (`BEGIN` … `COMMIT` that performed any write) may live
   in exactly **one group** for its whole lifetime;
2. the transaction binds to a group at the **first statement that touches data**
   (reads count too). After that:

   | Situation | Result |
   |-----------|--------|
   | same group | fine |
   | another group, **this transaction has not written yet** | becomes a "cross-group read-only" transaction, **allowed or not depends on the mode** (§3.3): `strict` errors, `loose` allows |
   | another group, **this transaction already wrote** | **immediate error** (write transactions never span groups = decision B) |
   | it has already spanned groups (read ≥2), **and now wants to write** | **immediate error** (crossing groups freezes the transaction as read-only) |

3. all of the above errors are raised **before the statement executes**, and the
   transaction stays rollback-able — not discovered at COMMIT;
4. an autocommit single statement only touches one table by construction (the
   grammar has no cross-table statement), so it is unaffected —
   `SELECT * FROM a; SELECT * FROM b;` is two independent statements and works
   as before.

### 3.2 The write slot (pessimistic single writer) → per group

Current semantics (see the header comment in
`storage/kv_engine/kv_engine.h`):

- `begin` opens a transaction **and takes a snapshot** (repeatable read);
- the write slot is **process-wide**; **acquiring it releases the snapshot**
  (the slot guarantees nobody else can commit, so "latest committed + my own
  buffer" is already a frozen view);
- a second connection's **write** cannot get the slot → `Status::Busy`;
- read-only transactions never take the slot, so **readers do not block writers**.

This design **keeps those semantics verbatim and only narrows the write slot's
scope from "the process" to "one group"**:

- the slot lives on the group (`RaftGroup`), so at most one write transaction
  per group at a time;
- because a transaction is pinned to one group, **"acquiring this group's slot
  releases the snapshot" carries over unchanged** (scope narrowed);
- as a result: **no read-set validation, no row locks, no 2PC** — single writer
  per group is serializable, by exactly the same argument as today.

> This is the main payoff of decision B: **banning cross-group transactions buys
> us out of distributed transactions entirely.** The cost is that explicit
> transactions cannot span tables — explicitly accepted.

### 3.3 Cross-group read-only transactions: `strict` / `loose`

**Why a mode is needed**: different groups commit at different times, so a
group-spanning "snapshot" is **not** a single global snapshot — it can observe a
state that never actually existed. Rather than deciding for the user, let the
client choose explicitly.

| Mode | Cross-group read-only transaction | Semantics |
|------|-----------------------------------|-----------|
| `strict` (**default**) | **rejected** with a clear error | does not pretend to offer a globally consistent snapshot |
| `loose` | allowed | **one snapshot per group** (taken when that group is first read) — **not a globally consistent snapshot** |

Things worth spelling out:

- in P1/P2 `strict` simply means "reject" and **needs no extra machinery**. A
  genuinely globally consistent snapshot requires a global timestamp / MVCC
  (see §11) and is explicitly out of scope;
- `loose` costs almost nothing: `begin` takes **no** snapshot; each group's
  snapshot is taken when that group is first read, so the transaction holds
  **N per-group snapshots**;
- the **resource implication must reach the user docs**: a long-lived loose
  transaction pins old leveldb versions in several groups (the same problem as
  today's "idle transaction pins versions", just in more places).
  `idle_in_transaction_timeout_ms` applies to it as well;
- a `loose` transaction still takes **no write slot**, so readers do not block
  writers;
- **once it has written, it may not cross groups any more** (last two rows of
  §3.1) — otherwise it becomes a cross-group write transaction again.

### 3.4 Protocol change: the client declares loose / strict

- **Default is `strict`** (safe); relaxing requires an explicit client opt-in.
- The setting is **per connection**, not per transaction. Reason: the grammar
  explicitly rejects transaction modes today (`transaction modes are not
  supported (use plain BEGIN)` in `sql.y`), a connection-level setting needs
  **no SQL grammar change** and maps to a single CLI flag; also, mixing modes
  across transactions on one connection makes "repeatable read" harder to state
  precisely.
- Today `HELLO` is server→client only (`u16 proto_version, u16 server_version,
  u32 capabilities`) — there is **no client→server options channel**. Add:

  ```
  CLIENT_OPTIONS  client->server (sent once, after HELLO)
    u32 capabilities          // bitfield; new bit kCrossGroupReadLoose
    u16 option_count
    { u16 key_len, bytes key, u16 value_len, bytes value } * option_count
  ```

  Extensible key/value, so future options need no frame change. **Capability
  negotiation**: the server advertises `kCrossGroupReadLoose` in HELLO; the
  client only sends the option when it sees the bit, otherwise it stays strict.
  An old server treats the unknown frame as a protocol error (disconnect), an
  old client never sends it → default strict; neither side silently runs with
  the wrong semantics.
- CLI: `sqldb-client --cross-group-read=loose|strict` (same flag for local
  `sqldb`).
- Where it lives: `session::Session` holds the mode; the **enforcement point is
  `RaftKVEngine`** — it sees every key/range and the routing table, so it can
  also remember which groups this transaction has touched. A violation returns
  `Status::CrossGroupTransaction`, which the server turns into an ERROR frame.

### 3.5 Concurrency payoff

Writes to different groups **run in parallel** (each with its own leader and
slot). That is the real improvement over today's process-wide single writer, and
the fundamental reason to build multi-raft.

## 4. Components and interfaces

### 4.1 `raft/` (knows nothing about SQL)

```cpp
namespace raft {

struct NodeId { uint64_t value; };

struct LogEntry {
  uint64_t index;
  uint64_t term;
  std::string data;      // decision A: this is a serialized kv::WriteBatch
};

struct HardState {       // term/vote must be durable before acking
  uint64_t term = 0;
  std::optional<NodeId> voted_for;
};

// Persistence: term/vote + log segments. A separate LevelDB is recommended,
// decoupling raft metadata from business data.
class LogStore {
  virtual std::expected<void, Error> append(const LogEntry &);
  virtual std::expected<LogEntry, Error> at(uint64_t index);
  virtual std::expected<void, Error> truncate_suffix(uint64_t from);
  virtual std::expected<void, Error> save_hard_state(const HardState &);
  virtual std::expected<HardState, Error> load_hard_state();
};

// State machine: implemented by the SQL side (applies batches to the local KV)
class StateMachine {
  virtual std::expected<std::string, Error> apply(const LogEntry &) = 0;
  virtual std::expected<std::string, Error> snapshot(KeyRange) = 0;   // serialize
  virtual std::expected<void, Error> restore(std::string_view) = 0;
};

// Transport: svrkit in production; in-process for tests (the sandbox denies
// bind(), so tests must be able to run without sockets)
class Transport {
  virtual void send(NodeId to, const Message &) = 0;
  virtual void on_message(std::function<void(NodeId, Message)>) = 0;
};

class RaftNode {                 // one group
  void tick();                   // clock-driven: election / heartbeat
  std::expected<Proposal, Error> propose(std::string data);  // leader only
  bool is_leader() const;
  Role role() const;             // Follower / Candidate / Leader
  uint64_t term() const;
  uint64_t commit_index() const;
  uint64_t applied_index() const;
  std::optional<NodeId> leader_hint() const;
};

class MultiRaft {                // routing + many groups
  RaftNode *group_for(const kv::Key &);
  std::expected<Proposal, Error> propose(const kv::WriteBatch &);  // single group
  std::vector<GroupInfo> groups() const;
};

} // namespace raft
```

### 4.2 The new SQL-side implementations

```cpp
class RaftKVStore : public kv::KVStore {
  std::shared_ptr<kv::KVStore> local_;   // local LevelDB, used as the state machine
  raft::MultiRaft raft_;
};

class RaftKVEngine : public kv::KVEngine {
  // get / scan            -> read the locally applied state (read-index on the leader)
  // write_batch / commit  -> route to group -> propose -> wait for commit -> apply
};
```

Two new `kv::Status` values:

- `NotLeader` (carrying a `leader_hint`) — the server answers with an ERROR
  frame and the client reconnects to the leader;
- `CrossGroupTransaction` — transaction escaped its group (decision B), carrying
  both group ids for diagnosis. Two cases: a **write transaction spanning
  groups** (rejected in every mode), or a **cross-group read-only** transaction
  under `strict`. The enforcement point is `RaftKVEngine` (it sees the keys and
  the routing table), not the SQL layer.

**The SQL protocol does not change**: `NotLeader` travels over the existing
ERROR frame.

## 5. Read/write paths

### 5.1 Writes (`COMMIT` / autocommit statements)

```
statement executes -> WriteBatch (already exists)
  -> route: every key in the batch must fall in one group, else CrossGroupTransaction
  -> hold that group's write slot? (else Busy)
  -> is this node the leader of that group?
       no  -> NotLeader (+hint)
       yes -> LogStore.append(entry)           (durable first)
           -> replicate to a majority
           -> commit_index advances
           -> StateMachine.apply(batch)        (into the local LevelDB)
           -> OK (affected_rows comes from apply's return value)
```

### 5.2 Reads

- **P1: reads and writes both go to the leader**;
- a read on the leader must first pass a **read-index** barrier: record the
  `commit_index` at request time and wait for `applied_index >= it` before
  reading locally — otherwise a just-committed write may not be visible;
- follower read / lease is a P3 concern (out of scope).

### 5.3 Idempotency

`put` / `remove` are individually idempotent, but a batch derived from a read
**is recomputed on retry** and is then no longer the same batch. So log entries
must carry a **client request id**, and the state machine remembers the last
applied id per client, returning the previous result for duplicates.

## 6. Snapshots

- when a follower falls too far behind (or the log is compacted) the leader
  ships the whole group range;
- produce: `scan` over `[start_key, end_key)` (the existing `Iterator` suffices);
- receive: clear that range, install the snapshot, and **catch up while
  installing** (entries arriving mid-install must be buffered/replayed);
- the `@system/*` meta group needs snapshots too.

## 7. Configuration

A new `[raft]` section (reusing `server::Config` / `validate()` / `Logger`):

```ini
[raft]
enabled = false
node_id = 1
listen  = 127.0.0.1:5434
peers   = 1@127.0.0.1:5434,2@127.0.0.1:5435,3@127.0.0.1:5436
election_timeout_ms = 1000
heartbeat_ms        = 100
log_path            = ./sql_db_raft_log     # separate LevelDB for the raft log
# static shards (hand-written until P3, then owned by the _meta group)
shard.0 = ,+                                 # [empty, +∞) = everything
```

## 8. Reuse checklist (don't rebuild what exists)

| Need | Use |
|------|-----|
| raft transport (inbound) | `svrkit::TcpServer` + your own `ConnectionHandler`, on a **dedicated port** |
| raft transport (outbound) | `common/net::TcpSocket::connect`; keep **one long-lived connection per peer** instead of a blocking connect per RPC |
| raft core / apply thread | `ServiceThread` + `SubmitToService` (**never put the state machine on the coroutine layer**) |
| heartbeat / election / cross-thread wakeup | `Loop::add_timer` / `Loop::post` (the clock must be injectable, see §9) |
| config / logging / metrics | `server::Config`, `server::Logger`, the `Metrics` shape |
| log entry payload / snapshot / apply | `kv::WriteBatch`, `kv::Iterator`, `scan`, `prefix_end` |

**Why a dedicated port for raft**: SQL and raft have completely different
timeout, auth and backpressure profiles; sharing one port requires a first-byte
demultiplexer in the SQL protocol, turning the SQL frame parser into a mover of
internal traffic.

## 9. Phased plan

| Phase | Content | Verification |
|-------|---------|--------------|
| **P0** | raft core: election / log replication / commit / apply / snapshot; **in-proc transport + injectable fake clock** | unit tests: happy path, partitions, dropped messages, reordering, restart, single-node → three-node. **Fully green inside the sandbox** (no sockets) |
| **P1** | single group wired into SQL: `RaftKVStore` + static placement (1 node → 3 nodes) + leader-only reads/writes + read-index + idempotency id | `test_raft` + end-to-end: SQL on raft; kill the leader and watch a new one take over |
| **P2** | many groups: shard per table; `@system/*` in group 0; independent leader per group; **reject cross-group write transactions + `strict`/`loose` modes + the `CLIENT_OPTIONS` frame** (§3.3/§3.4) | two tables write concurrently without blocking each other; a cross-group write transaction fails clearly; under `--cross-group-read=loose` `BEGIN; SELECT a; SELECT b; COMMIT` runs, under `strict` (default) it errors |
| **P3** | `_meta` group owns placement, **table-level split/merge**, membership changes, follower read / lease | needs its own design (not in this document) |

**Start with P0**: it does not touch SQL, is deterministically testable, and
stays green inside the sandbox. The three interfaces P0 freezes (`LogStore`,
`StateMachine`, `Transport`) determine how smoothly P1 wires into SQL.

## 10. Pitfalls (by severity)

1. **`@system/*` must be replicated too.** `CREATE DATABASE/TABLE` writes meta
   keys, not `@data/*`. Forget this and you get "schema exists on only one
   node". Pin them to group 0.
2. **The global write slot must be split per group.** `KVStore::write_slot_held()`
   is currently process-wide; without splitting it, many groups buy you nothing.
3. **Snapshot release must follow the new scope**: today it is "acquiring the
   process-wide slot releases the snapshot". It becomes "acquiring *this group's*
   slot releases *this group's* snapshot". Copying it as "any slot releases all
   snapshots" breaks repeatable read.
4. **term/vote and the log must be durable before acking**, otherwise a power
   cut can cause unstable elections or lost acknowledged writes; align with the
   existing `WriteBatch::set_sync(true)` semantics.
5. **Idempotency**: retries recompute the batch (§5.3), so a client request id
   and dedup are mandatory.
6. **`remove_range` must land in a single group** — another reason for
   one-table-one-group.
7. **The clock must be injectable**: heartbeats/elections eventually use
   `Loop::add_timer`, but P0 must be able to run deterministically without a
   real clock.
8. **Do not put the raft state machine on the coroutine/Loop thread** —
   consistent with the existing conclusion in
   `tests/test_server/codex_check_issues.md`.
9. **`loose` pins old versions in several groups at once**: one live loose
   transaction = N per-group snapshots. The idle timeout must apply to it, and
   the user docs must say so.
10. **The cross-group check must happen before the statement executes**, and the
    enforcement point is `RaftKVEngine` (only it knows the group boundaries).
    Deciding in the SQL layer would miss the `@system/*` routing table and would
    duplicate the "which groups have I touched" state.

## 11. Open questions

- **A real `strict`**: today `strict` means "reject", not "globally consistent
  snapshot". Delivering the latter needs a global timestamp (HLC / central
  time) plus MVCC (versioned reads) — an independent large feature. The default
  for `strict` should be revisited then.
- Should `loose` be split further (e.g. "read latest per group" vs "snapshot per
  group")? Only snapshot-per-group is planned for now.
- Log compaction policy: by entry count or by bytes? How often to snapshot?
- Who retries after `NotLeader`: the CLI reconnecting, or the server proxying?
  (P1 keeps it simple: return the leader hint to the client.)
- Whether P3 needs follower read / lease depends on whether read amplification
  becomes a bottleneck.
- After table-level split/merge, the "one transaction, one group" rule must be
  revisited (a table spanning two groups makes it a cross-group transaction
  again).
