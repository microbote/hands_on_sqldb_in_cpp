# raft module design: Multi-Raft for sqldb

中文版：[DESIGN.md](DESIGN.md)

**Status: design frozen; P0, the P1a adapter and P1b are implemented** (election
/ log replication / commit / apply / LevelDB persistence / payload codec / KV
state-machine bridge / `RaftKVStore` + `RaftKVEngine` / ReadIndex barriers /
per-group write slot / `RaftRuntime` single thread / RPC codec / TCP transport /
`[raft]` config and the `sqldb-server` wiring). Remaining work is listed in §11
(snapshot transfer, compaction, read-index batching, cross-group read modes,
membership changes, P2 multi-group). The **NotLeader + leader-hint redirect**
(`[raft] sql_endpoints` → ERROR-frame hint → one client reconnect+retry) is in.
This is an implementation plan, not a user guide;
responsibilities / interfaces / pitfalls live in `raft/README.md`.

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

class RaftRuntime {              // P1b: the only thread allowed to touch RaftNode
  void start(size_t queue_max);
  void stop();                   // drain the queue, then join
  std::expected<void, Error> run(std::function<void()>);        // blocking submit
  std::expected<Proposal, Error> propose(std::string data);     // blocking submit
  std::expected<ReadIndex, Error> read_barrier();               // blocking submit
  bool post_message(NodeId, const Message &);                   // fire and forget
  bool request_tick();                                          // fire and forget
};

} // namespace raft
```

`message_codec.{h,cpp}` provides the binary RPC frame (versioned, length
prefixed, with a streaming decoder) and `peers.{h,cpp}` parses the
`id@host:port` list of `[raft] peers`. Neither touches a socket, so both are
fully unit-testable inside the sandbox.

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

### 4.3 The P1 adapter: what is implemented (`raft/raft_kv_store.{h,cpp}`)

The single-group version turned the §4.2 sketch into two classes. **P1 has one
group, so `RaftKVEngine` currently holds a `RaftNode &` directly**; P2 replaces
that one seam with `MultiRaft::group_for(key)` routing (see §11).

| Type | Responsibility |
|------|----------------|
| `RaftKVStore` | implements `kv::KVStore`; `connect()` hands each session a `RaftKVEngine`; `open/close/flush/stats` proxy the local store |
| `RaftKVEngine` | implements `kv::KVEngine`; reads go through a read-index barrier plus the locally applied state, writes go propose → wait for commit + apply, and an explicit transaction is replicated as one batch at `COMMIT` |

Contract, ordered by importance:

1. **Writes only enter through `connect()`.** `RaftKVStore::write_batch()`
   returns `NotSupported`: the raw store write path belongs to the state
   machine (which owns the local store directly). SQL writes must be proposed,
   or they silently bypass replication. Raw *reads*
   (`RaftKVStore::new_iterator()`) serve applied local state and are safe to
   proxy (snapshot generation needs them).
2. **Every read passes a read-index barrier** (algorithm in §5.2); no leader,
   or no quorum, maps to `NotLeader` / `Timeout`.
3. **A transaction is a local snapshot plus the group write slot.**
   `begin_transaction()` takes a snapshot on the local store (repeatable read);
   the first write takes the group write slot and then releases the snapshot
   per §3.2; `COMMIT` encodes `TxBuffer::to_batch()` into **one** proposal. A
   failed `COMMIT` leaves the transaction open and rollbackable, with no local
   state mutated.
4. **Read your own writes**: inside a transaction, `get/exists/get_batch/
   new_iterator` consult the `TxBuffer` overlay before the applied state, so
   the engine behaves like the local one.
5. **An empty transaction `COMMIT` writes no log entry.**
6. **Idempotency**: every proposal carries `(client_id, request_id)`;
   `request_id` increases monotonically within a connection and a retry
   **must reuse the same id** so the state machine can deduplicate.
7. **A timeout means "unknown outcome", not "not written"**:
   `Proposal::wait_for()` timing out only says the commit was not observed; the
   entry may still commit later. Callers retry with the same request id or
   surface the uncertainty — never assume an error means the write did not
   happen.
8. **Every wait is bounded**: both the read barrier and proposal waits use
   `election_timeout_ms` as their budget. A partitioned old leader never steps
   down on its own, so an unbounded wait hangs the session
   (`IsolatedLeaderCannotServeReads` and
   `IsolatedLeaderFailsReadsAndWritesWithTimeout` cover both paths).

Error mapping:

| raft error | `kv::Status` | Caller behavior |
|------------|--------------|-----------------|
| `NotLeader` | `NotLeader` | answer an ERROR frame; P1 reports, it does not forward (§11) |
| `Timeout` (read barrier / proposal wait) | `Timeout` | reads may be retried; writes must retry with the same request id |
| `InvalidArgument` / `IOError` / `InternalError` | same name | propagate |

**Threading rules** (P1 must respect these or it is a data race):

- `RaftNode::tick()/handle_message()/propose()/read_barrier()` must all be
  called on the **same Raft service thread**;
- `RaftKVEngine` writes block on a completion and therefore **must not block on
  that thread**;
- the intended shape is the `RaftRuntime` of §9: session/write threads
  `SubmitToService` a proposal, and the Raft service thread drives tick /
  messages / commit / apply and finishes the completion; `Transport::send()`
  still only enqueues and never re-enters synchronously.
- Today `raft_kv_store` is a thread-agnostic skin that assumes its caller
  already serializes access. That holds for the single-threaded tests; the
  runtime has to exist before this is wired to the server.

### 4.4 P1b: RaftRuntime, transport and the threading model (landed)

Which thread touches what:

| Thread | Owns | How it talks to RaftNode |
|--------|------|--------------------------|
| **Raft service thread** (`RaftRuntime`) | the only toucher of `RaftNode`, `StateMachine` and `LogStore` | calls `tick/handle_message/propose/read_barrier` directly |
| transport receive thread (svrkit Loop) | sockets, frame buffers | only `post_message()`, never `handle_message()` |
| transport send thread | one long-lived connection per peer | only reads the send queue |
| SQL session / write service threads | `RaftKVEngine`, `TxBuffer` | blocking submits `propose()/read_barrier()`, then waits on the completion |
| timer (`Loop::add_timer` or the test fake clock) | nothing | `request_tick()` |

Rules and errors:

- **Blocking submits** (`run/propose/read_barrier`) enqueue a lambda, let the
  service thread run it, and return; a `Proposal` returned by `propose` is then
  waited on by the **caller's** thread (§4.3 timeout semantics). The service
  thread is never blocked by a caller.
- **Posts** (`post_message/request_tick`) are fire-and-forget; a full queue
  drops the item and counts it (a dropped tick is fine — the next heartbeat
  period catches up).
- A blocking submit issued *from* the service thread returns `Busy` instead of
  deadlocking; a full queue also returns `Busy`, which the adapter maps to
  `kv::Status::Busy` (retryable).
- `stop()` drains queued ticks/messages and only then joins, so messages that
  were already received are not silently lost; afterwards `running()` is false
  and further submissions fail.

Timers: production arms `Loop::add_timer(heartbeat_ms, ...)` on a loop that
only drives timers and calls `request_tick()` (`add_timer` is one-shot, so the
callback re-arms); tests inject a fake clock and post ticks manually, so
elections and heartbeats stay deterministic.

**Transport topology** (`raft/tcp_transport.{h,cpp}`): every node dials every
peer, and the receive side uses `svrkit::TcpServer`, so a pair of nodes has
**two** connections (one per direction). That looks wasteful, but it buys "each
fd is owned by exactly one thread":

- outbound sockets belong to the sender thread (blocking connect/write), so
  `send()` only enqueues and never has to hop onto another thread's loop;
- inbound sockets belong to the `TcpServer` loop (non-blocking reads), and
  decoded frames are only handed to `RaftRuntime::post()` — they never touch
  `RaftNode`;
- the dialer sends a one-frame **handshake** (`u8 kind + u64 node id`) so the
  acceptor learns who connected (svrkit does not expose the peer address).

On a write failure the unsent bytes are **kept** and resent after a reconnect
with backoff: Raft RPCs already tolerate duplicates (AppendEntries is retried),
so the transport is **at-least-once**, not exactly-once. A full outbound queue
or a rejected post drops the frame and counts it.

**Server wiring** (`server/raft_bootstrap.{h,cpp}` + `main_server.cpp`):
local KVStore → `LevelDBLogStore(<log_path>/log)` →
`LevelDBRequestResultStore(<log_path>/request_results)` → `KVStateMachine` →
`RaftTcpTransport` → `RaftNode` → `RaftRuntime` → listen + inbound thread +
heartbeat timer → `RaftKVStore`; shutdown is the reverse (timer → transport →
runtime → stores). With `raft.enabled = false` (the default) the startup path is
exactly as before.

Two implementation details worth remembering:

- **A single-member group never listens**: no peer can connect, so skipping the
  listener lets single-node raft run anywhere (including sandboxes that deny
  bind); `RaftBootstrap::Options::bind_listener` is the explicit test knob.
- **`client_id` carries a per-process random salt**: the idempotency table is
  durable and the state machine skips a proposal whose `(client_id,
  request_id)` it has already applied. A plain counter repeats after a restart,
  so the first new write after a restart could be mistaken for a replay and
  silently do nothing.

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
- a read on the leader must first pass a **ReadIndex** barrier (implemented
  algorithm):
  1. record the `commit_index` at request time as the **read target**;
  2. immediately start a heartbeat round (AppendEntries) and number it `R`;
  3. only consider leadership valid once a majority has acknowledged a round
     that started **at or after this request** (`peer_acked_round >= R`);
  4. wait for `applied_index >= read target`, then read the locally applied
     state.
- **Why steps 2/3 cannot be skipped**: a partitioned old leader still believes
  it leads. Reusing earlier acks, or waiting only for the no-op that committed
  at election time, lets it keep serving stale reads — the "stale leader" trap
  in §10. Only rounds started after the read request count, so late replies to
  older rounds are never accepted as proof. To make that rule executable, the
  AppendEntries request carries its round and the response echoes it: the
  leader only credits `response.round` into `peer_acked_round`, so a late reply
  to an older round simply does not match the new round number.
- **Why the current-term no-op is not the criterion**: the no-op proves
  leadership only once, right after the election; a later partition produces no
  new no-op, so waiting for it means trusting an expired proof forever. The
  no-op stays for its real job: letting a new leader safely commit entries
  inherited from a previous term.
- **Cost and follow-up**: today every read (and every key) costs a full
  heartbeat round. Batching a statement's or transaction's reads into one
  confirmation needs a time bound — a lease (with clock assumptions) or an
  injected clock. See §11.
- no quorum within one `election_timeout_ms` maps to `Timeout`;
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
# client-reachable SQL addresses (id@host:port), used to redirect NotLeader
sql_endpoints       = 1@127.0.0.1:5433,2@127.0.0.1:5434,3@127.0.0.1:5435
```

**Landed** (`server/config.{h,cpp}`): all of these keys live in the built-in
default table (`enabled = false` by default, so the startup path is unchanged
when raft is off) and `ServerConfig` exposes `raft_enabled()/raft_node_id()/
raft_peers()/raft_listen{,_host,_port}()/raft_election_timeout_ms()/
raft_heartbeat_ms()/raft_log_path()`.

`validate()` rules:

- `heartbeat_ms < election_timeout_ms` (checked whether raft is on or off);
- a non-empty `peers` is always parsed: `<node_id>@<host>:<port>`, id >= 1, port
  in 1..65535, unique ids and unique endpoints (checked **even while disabled**,
  so a typo cannot hide until the day raft is switched on);
- with `enabled = true` it additionally requires a non-empty `peers`, `node_id`
  present in `peers`, `listen` shaped like `host:port`, and a non-empty
  `log_path`.
- `sql_endpoints` is optional (same `id@host:port` syntax); every id must exist
  in `peers` or validation fails (a typo would silently disable redirects).
  Empty = answer `NotLeader` without a reconnect target.

Static shards (`shard.N = <start>,<end>`) are not implemented yet: P1 has a
single group, and P2 introduces the static range table with `@system/*` pinned
to group 0 (§2.2).

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

| Phase | Content | Verification | Status |
|-------|---------|--------------|--------|
| **P0** | raft core: election / log replication / commit / apply / snapshot; **in-proc transport + injectable fake clock** | unit tests: happy path, partitions, dropped messages, reordering, restart, single-node → three-node. **Fully green inside the sandbox** (no sockets) | landed |
| **P1a** | single-group adapter: `RaftKVStore` / `RaftKVEngine`, **ReadIndex barrier**, per-group write slot, proposal idempotency id, timeout and error mapping | the `RaftKVAdapter` suite in `test_raft` (single-node read/write, transactions, rollback, write slot; three-node replication; follower `NotLeader`; partitioned-leader timeout) | landed |
| **P1b** | `RaftRuntime` threading model (`ServiceThread` driving tick/messages/apply), RPC codec, `[raft]` config, production transport (svrkit on its own port), server entry wiring, leader hint back to the client | end-to-end: SQL on raft; kill the leader and watch a new one take over | landed (single-node restart persistence, a three-node real-socket election/replication test, and NotLeader + redirect tests) |
| **P2** | many groups: shard per table; `@system/*` in group 0; independent leader per group; **reject cross-group write transactions + `strict`/`loose` modes + the `CLIENT_OPTIONS` frame** (§3.3/§3.4) | two tables write concurrently without blocking each other; a cross-group write transaction fails clearly; under `--cross-group-read=loose` `BEGIN; SELECT a; SELECT b; COMMIT` runs, under `strict` (default) it errors | not started |
| **P3** | `_meta` group owns placement, **table-level split/merge**, membership changes, follower read / lease | needs its own design (not in this document) | not started |

**Start with P0**: it does not touch SQL, is deterministically testable, and
stays green inside the sandbox. The three interfaces P0 freezes (`LogStore`,
`StateMachine`, `Transport`) determine how smoothly P1 wires into SQL.

P1 is split into a/b because everything in P1a is verifiable with **one thread
and the in-proc transport** (done today), while P1b introduces threads and
thereby turns `RaftNode`'s "single caller thread" assumption into a hard
constraint that has to be designed together with the runtime. The adapter must
not be wired into the server before that runtime exists.

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
11. **ReadIndex must be confirmed by a heartbeat round started *after* the read
    request.** Reusing earlier acks, or waiting only for the election-time
    no-op, lets a partitioned old leader serve stale reads. Test:
    `IsolatedLeaderCannotServeReads`.
12. **Every cross-thread wait needs a deadline.** A partitioned old leader never
    steps down, so `wait()` without a timeout hangs the session forever. Test:
    `IsolatedLeaderFailsReadsAndWritesWithTimeout`.
13. **A proposal timeout is not a failed write**: the entry may commit later.
    Retries must reuse the same `(client_id, request_id)` so the state machine
    deduplicates; otherwise one timeout plus retry executes the write twice.
14. **`RaftKVStore::write_batch()` must stay rejected** (`NotSupported`).
    Keeping a raw write path around propose is a silent local-write backdoor
    into a replicated store.
15. **The adapter must not spawn threads, and must not block on the Raft
    service thread waiting for a proposal.** See the threading rules in §4.3;
    this is the first rule to get violated when wiring the server.
16. **The transport thread may only `post_message()`, never
    `handle_message()`.** Handling a frame inline means the network thread and
    the Raft service thread both touch `RaftNode`, and it pushes slow disk/apply
    backpressure onto the network thread.
17. **`[raft] enabled = true` must fail loudly until the wiring exists.**
    Config validation passing does not mean replication is on; silently falling
    back to local writes is the worst kind of bug (the user believes the write
    was replicated).
    (The wiring exists now; keep this rule for future partial integration —
    failing startup beats pretending.)
18. **One connection per direction in the transport**: do not "save" an fd by
    making the receive thread write on another thread's loop; that breaks the
    "each fd has exactly one owning thread" invariant, and cross-thread socket
    writes are the hardest races to debug.
19. **`client_id` must be unique across restarts** (a per-process random salt
    today). The idempotency table is durable, and a repeated id makes the first
    new write after a restart look like a replay and silently skip it — the
    symptom is "the write reported OK but the value did not change".
20. **Startup order matters**: `RaftNode::start()` (which installs the transport
    callback) must happen before the inbound thread starts, or the first
    messages are dropped because the callback is not registered yet.

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
- **ReadIndex batching / lease**: every key currently costs one confirmation
  round, which shows up as read amplification. Batching it per statement needs
  a time bound (lease + clock assumptions, or an injected clock); this is the
  first performance task after P1b.
- **Structured errors**: `NotLeader` needs a leader hint and
  `CrossGroupTransaction` needs both group ids, but today there is only the
  `kv::Status` enum. Either widen the error type (`StatusInfo`) or add fields to
  the ERROR frame.
- **`RaftRuntime` details**: "one service thread + blocking submits + posts" is
  settled (§4.4); still open is whether apply gets its own thread (today apply
  runs on the service thread, so a slow `remove_range` delays heartbeats) and
  whether the proposal queue should be split per group.
- **Transport operations**: reconnect is "backoff + resend unsent bytes"; there
  is no keepalive/half-open detection (a wedged peer is only noticed when a
  write fails) and no load testing of the connection count (N×(N-1) today).
  Whether to multiplex is a question for real measurements.
- **`NotLeader` leader hints and retries**: clients currently only see the
  server now checks the hint once per statement (a cheap blocking submit to the
  raft service thread), answers `NOT_LEADER` + endpoint on a follower, and the
  client reconnects and retries once. Not implemented: **server-side
  forwarding** (the server running the statement on the leader itself), which
  would need an internal client — deferred until there is a clear payoff.
- **NotLeader on the schema read path**: the `Catalog` read API returns
  `bool`/`optional` (it cannot distinguish "no such table" from "read failed").
  With a known leader the server pre-check catches followers first, so this only
  bites a follower that has not learned the leader yet (fresh start / partition)
  — it may report "table not found" or treat the table as empty. Fixing it
  properly means giving the catalog reads a status/tri-state.
- **Configurable proposal deadline**: it borrows `election_timeout_ms` today;
  eventually there should be a dedicated `raft.proposal_timeout_ms`, with read
  and write timeouts distinguished.
- **Atomicity of affected rows / apply results with idempotency results**:
  `apply_result` is currently the constant `"applied"`. Making it real affected
  rows requires persisting it atomically with the request result, or a restart
  replay returns a different value.
