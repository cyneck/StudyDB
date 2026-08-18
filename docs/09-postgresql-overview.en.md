# Chapter 9 · PostgreSQL Panorama

> Corresponding source files: none (this is the closing chapter; it doesn't cover our code, but pulls the lens back to a real database)
>
> This is the project's **finale** — placing the small engine from the previous eight chapters into the perspective of a real database (PostgreSQL), to see what we did, what we didn't do, and why a production-grade database is so much more complicated.

---

## 9.1 What we did, and what PostgreSQL additionally does

Across the previous eight chapters our small engine implemented:

| Layer | Component | Problem solved |
|-------|-----------|----------------|
| Storage | `storage_mgr` | page file I/O |
| Buffer | `buffer_mgr` | FIFO / LRU / LRU-K replacement |
| Records | `record_mgr` | tables / records / scans |
| Index | `btree_mgr` | B+ tree point & range lookup |
| DDL | `ddl_parser` | CREATE / DROP TABLE |
| DML | `dml_parser` | INSERT / UPDATE / DELETE / SELECT |
| Metadata | `catalog` | centralized schema registry |

This is a "runnable" engine, but it is **single-connection, single-threaded, no concurrency, no recovery**. If the process crashes, all unflushed data is lost; if two clients write the same table at once, there is no isolation; and SQL is executed "as written" with no optimization.

PostgreSQL, on top of the same layers, additionally does:

- **Query optimization**: SQL is not executed "as written"; it is translated into many candidate plans and the cheapest is picked
- **Concurrency control**: Multi-Version Concurrency Control (MVCC) lets reads not block writes and writes not block reads
- **Crash recovery**: Write-Ahead Logging (WAL) guarantees that once a transaction commits, it survives power loss
- **Background daemons**: BG Writer, Checkpointer, WAL Writer keep working in the background to reduce query latency
- **Process model**: every connection gets its own OS process, supervised by the postmaster

---

## 9.2 PostgreSQL's component panorama

PostgreSQL's components, by layer:

```
┌──────────────────────────────────────────────────────────┐
│  client connection                                       │
├──────────────────────────────────────────────────────────┤
│  Query Layer                                             │
│   Parser → Analyzer/rewriter → Planner → Executor        │
│   (SQL→AST) (semantic + view rewrite) (cost-based) (Volcano) │
├──────────────────────────────────────────────────────────┤
│  Storage Layer                                           │
│   Heap tables · shared_buffers · WAL                     │
├──────────────────────────────────────────────────────────┤
│  Transaction Layer                                       │
│   MVCC · Lock Manager · WAL Writer · Checkpointer        │
│   · BG Writer                                            │
├──────────────────────────────────────────────────────────┤
│  Process Model                                           │
│   postmaster  ←→  one backend process per connection     │
└──────────────────────────────────────────────────────────┘
```

| Component | Role | In our engine |
|-----------|-------|----------------|
| Parser | SQL → AST | `ddl_parser` / `dml_parser` |
| Analyzer | semantic analysis, view rewrite, name binding | our catalog lookup |
| Planner | cost-based optimizer | **none** |
| Executor | Volcano pull model | `record_mgr` scan |
| Heap AM | row-based table storage | `record_mgr` page layout |
| shared_buffers | shared buffer pool | `buffer_mgr` |
| nbtree | Lehman-Yao-style B-tree access method | `btree_mgr` (B+ tree-like teaching index) |
| WAL | write-ahead log | **none** |
| MVCC | multi-version concurrency | **none** |
| Lock Manager | table / row locks | **none** |
| BG Writer | background page flusher | **none** |
| Checkpointer | periodic checkpoints | **none** |
| pg_catalog | system catalogs | `catalog` |

---

## 9.3 Query Optimizer: why the same SQL can run in many ways

Consider:

```sql
SELECT * FROM orders o JOIN users u ON o.uid = u.id WHERE u.age > 18;
```

There are at least three ways to run it:

1. **Nested loop join**: for each row of `orders`, scan `users` for matches — O(n×m)
2. **Hash join**: build a hash table on `users`, probe with `orders` — O(n+m)
3. **Merge join**: sort both sides on the join key and merge — O(n log n + m log m)

Which is fastest depends on data size, indexes, memory, and statistics. PostgreSQL's **Planner**:

1. Enumerates candidate plans (join order, join method, scan method)
2. Estimates each plan's cost from statistics (row counts stored in `pg_class.reltuples` and friends)
3. Picks the cheapest

The cost unit is "cost of one sequential page read = 1.0"; everything else is scaled relative to that. This is why the `ANALYZE` command matters — it refreshes statistics, otherwise the planner chooses based on stale data. You can inspect a plan with `EXPLAIN`, and `EXPLAIN ANALYZE` actually runs the query and reports real timings.

---

## 9.4 Executor Volcano model: every operator is open / next / close

PostgreSQL's executor uses the **Volcano model**: every operator (scan, filter, join, agg, sort…) implements three calls:

```
open()      ← initialize
next()      ← pull one row (or batch)
close()     ← clean up
```

Data flows **pull-style**: upper operators call lower operators' `next()`, and lower operators passively produce a row. Example:

```
SELECT name FROM users WHERE age > 18;

Limit
  ↑ next()
Filter (age > 18)
  ↑ next()
SeqScan on users
  ↑ next()
Buffer pool ← disk
```

Pros: simple to implement, low memory (one row at a time), easy to compose arbitrary operator trees. Cons: a function-call chain per row and poor CPU-cache locality. Some modern engines use **vectorized execution** to amortize this overhead. PostgreSQL's standard executor remains primarily tuple-at-a-time; LLVM JIT compiles expressions and tuple-deforming work but does not make the executor vectorized.

---

## 9.5 MVCC: multiple versions let reads not block writes

In our small engine, a scan reads pages directly with no notion of a "transaction". What if someone UPDATEs the same table concurrently? You either read inconsistent data or read a half-written row.

PostgreSQL's answer is **MVCC (Multi-Version Concurrency Control)**: UPDATE creates a new tuple version and marks the old version with `xmax`; DELETE marks the deleted version. Visibility is not the scalar rule "creator ID ≤ my ID < deleter ID". It is decided from the transaction snapshot (`xmin`, `xmax`, in-progress transaction IDs), tuple `xmin`/`xmax`, and commit/abort status; isolation level also determines when snapshots are acquired.

Effects:

- **Reads don't block writes**: readers see old versions, writers create new ones, no interference
- **Writes don't block reads**: while a new version is being written, the old one stays readable
- Cost: dead tuples accumulate and need **VACUUM** to reclaim space

MVCC is the foundation of PostgreSQL's high-concurrency throughput, and a key difference from MySQL/InnoDB — MySQL rebuilds old versions from the undo log, while PostgreSQL keeps multiple versions right in the table.

---

## 9.6 WAL: write the log first, then modify the data

What if power dies right after a transaction commits? Modified pages in memory haven't been flushed, but the client already got "COMMIT OK" — that violates **Durability**.

PostgreSQL's answer is **WAL (Write-Ahead Log)**: every modification is written to a WAL log file **before** it is written to the data file. The rule is simple:

> A data page may not be flushed to disk until the WAL record describing its modification has been flushed.

Recovery then just replays the WAL: starting from the last checkpoint's position, reapply every "committed but not-yet-flushed" modification. Commit also gets faster — you only need to `fsync` the WAL file (small, sequential) instead of fsyncing every data page (large, random).

Our small engine has no WAL — kill the process and everything is gone. This is exactly why our `demo` "loses" its data as soon as the process exits.

---

## 9.7 Checkpoint: shortening recovery time

Without checkpoints, crash recovery would replay the entire WAL from the beginning — potentially gigabytes. A checkpoint does:

1. Flush all dirty pages to data files
2. Write a `CHECKPOINT` record to the WAL noting the current WAL position (LSN)
3. WAL before that LSN can be recycled

Next crash recovery only replays from the most recent checkpoint's LSN — seconds to minutes.

In PostgreSQL the **Checkpointer** process triggers this periodically (`checkpoint_timeout` defaults to 5 min, or when `max_wal_size` of WAL has been written). Helpers:

- **BG Writer**: trickles dirty pages to disk so checkpoint doesn't cause an I/O spike
- **WAL Writer**: pre-flushes the WAL buffer to reduce fsync pressure at commit time

---

## 9.8 Process model: one process per connection

PostgreSQL's concurrency model is **process-per-connection**, not thread-per-connection:

```
Client A ─→ backend A ─┐
Client B ─→ backend B ─┼─→ shared memory (shared_buffers, WAL buffer, lock table)
Client C ─→ backend C ─┘         ↑
                          postmaster listens on the port,
                          fork()s each backend
```

The postmaster is the main process: it listens, accepts connections, and `fork()`s a backend child for each. Backends communicate through **shared memory** (shared_buffers, WAL buffer, lock table), not thread switches.

Pros: a crashing backend doesn't kill the whole server (postmaster just restarts it); easier to debug (one gdb per backend). Cons: processes are heavier than threads, so high connection counts cost memory (hence middleware like PgBouncer).

---

## 9.9 Evolution roadmap: our engine → PostgreSQL

| Our component | PostgreSQL equivalent | Evolution |
|---------------|----------------------|-----------|
| `storage_mgr` | md smgr (file abstraction) | more general, pluggable smgr |
| `buffer_mgr` | shared_buffers | cross-process, clock sweep replacement |
| `record_mgr` | Heap AM | pluggable table AMs (heap, columnar) |
| `btree_mgr` | nbtree | concurrent B-tree access method, deduplication |
| `catalog` | pg_catalog system tables | metadata stored as queryable tables |
| `ddl_parser` | Parser + Analyzer | full SQL standard, view rewrite |
| `dml_parser` | Parser + Analyzer | expressions, subqueries, CTEs |
| none | Planner | cost-based optimizer |
| none | Executor (Volcano) | composable operators, JIT |
| none | WAL | crash recovery |
| none | MVCC | multi-version concurrency |
| none | Lock Manager | table / row locks |
| none | BG Writer / Checkpointer | background flushing |

In one sentence: **we built the "skeleton"; PostgreSQL adds the "brain" (optimizer), the "immune system" (MVCC + Lock), the "memory" (WAL + Checkpoint), and the "organ coordination" (process model + background daemons).**

---

## 9.10 Recommended reading

1. **PostgreSQL Documentation** — `https://www.postgresql.org/docs/`, especially the "Internals" chapter (`Overview of PostgreSQL Internals`, `System Catalogs`). The authoritative source.
2. **The Internals of PostgreSQL** (online book, Hironobu Suzuki) — `https://www.interdb.jp/pg/`, illustrated guides to WAL, query processing, and the process model. Most beginner-friendly.
3. **Database Internals**, Alex Petrov (O'Reilly, 2019) — disk to transactions to distributed systems; the chapter layout is friendly for students who just finished a project like ours.
4. **Architecture of a Database System**, Hellerstein, Stonebraker, Hamilton (CACM 2007 survey) — the classic survey that explains "what subsystems make up a database" most clearly. Strongly recommended as the first thing to read.
5. **Readings in Database Systems** (Red Book, 5th ed.) — the canonical paper collection, with foundational papers for every sub-area.

---

## 9.11 Exercises

1. **Why does PostgreSQL choose "one process per connection" instead of "one thread per connection"?** Hint: think about stability, debugging, shared memory, and the OS's process / thread scheduling differences. What downsides does this choice bring? Why did MySQL choose the opposite path?

2. **MVCC lets "reads not block writes", but at what cost?** If a long transaction runs for an hour without committing, while others UPDATE lots of data, what happens? Hint: dead tuples, VACUUM, table bloat.

3. **Our small engine has no Planner — every SQL runs "as written". Give a concrete example where this is slow.** Hint: join order in a four-table join, pushing filters earlier vs. later, having indexes but the optimizer not knowing which to use.

---

> Back to [README](../README.en.md) for the project overview.
