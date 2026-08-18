# 第9章 · PostgreSQL 全景与高阶组件 PostgreSQL Panorama

> 对应源文件：无（这是收尾章节，不讲我们的代码，而是把视野拉到一个真实数据库上）
>
> 这是整个项目的**收尾**——把前 8 章实现的简易引擎放进一个真实数据库（PostgreSQL）的视野里，看看我们做了什么、还有什么没做、为什么一个生产级数据库要复杂那么多。

---

## 9.1 我们做了什么，PostgreSQL 还多做了什么

**中文**

回顾前 8 章，我们的简易引擎实现了：

| 层次 | 我们实现的组件 | 解决的问题 |
|------|---------------|----------|
| 存储 | `storage_mgr` | page file 读写 |
| 缓冲 | `buffer_mgr` | FIFO / LRU / LRU-K 替换 |
| 记录 | `record_mgr` | 表 / 记录 / 扫描 |
| 索引 | `btree_mgr` | B+ 树点查与范围扫描 |
| DDL | `ddl_parser` | CREATE / DROP TABLE |
| DML | `dml_parser` | INSERT / UPDATE / DELETE / SELECT |
| 元数据 | `catalog` | 表 schema 的集中登记 |

这是一个「能跑」的引擎，但它**只能单连接、单线程、无并发、无恢复**地执行 SQL。也就是说：进程一旦崩溃，所有未刷盘的数据全部丢失；两个客户端同时写同一张表，没有任何隔离机制；同一条 SQL 也只会按"写出来的样子"执行，不会优化。

PostgreSQL 在同样的层次之上，多做了几件大事：

- **查询优化**：SQL 不是「怎么写就怎么跑」，而是先翻译成多种执行计划，挑代价最小的
- **并发控制**：多版本并发（MVCC）让读不阻塞写、写不阻塞读
- **崩溃恢复**：预写日志（WAL）保证事务提交后即使断电也能恢复
- **后台守护**：BG Writer、Checkpointer、WAL Writer 等进程持续工作，减轻查询路径的延迟
- **进程模型**：每个连接独占一个进程，由 postmaster 主进程统一接管

**English**

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

## 9.2 PostgreSQL 的组件全景

**中文**

PostgreSQL 的组件按层次大致这样划分：

```
┌──────────────────────────────────────────────────────────┐
│  客户端连接  client connection                           │
├──────────────────────────────────────────────────────────┤
│  查询解析层  Query Layer                                 │
│   Parser → Analyzer/重写器 → Planner → Executor         │
│   (SQL→AST) (语义分析+视图重写) (代价优化器) (火山模型)  │
├──────────────────────────────────────────────────────────┤
│  存储引擎层  Storage Layer                               │
│   Heap 表 · 共享缓冲池 shared_buffers · WAL 日志         │
├──────────────────────────────────────────────────────────┤
│  事务与并发层  Transaction Layer                         │
│   MVCC · Lock Manager · WAL Writer · Checkpointer       │
│   · BG Writer                                            │
├──────────────────────────────────────────────────────────┤
│  进程模型  Process Model                                 │
│   postmaster 主进程  ←→  每个连接一个 backend 进程       │
└──────────────────────────────────────────────────────────┘
```

| 组件 | 作用 | 类比我们简易引擎 |
|------|------|------------------|
| Parser | SQL → AST 语法树 | `ddl_parser` / `dml_parser` |
| Analyzer | 语义分析、视图重写、绑定表名 | 我们的 catalog 查找 |
| Planner | 基于代价的优化器 | **没有** |
| Executor | 火山模型拉取式执行 | `record_mgr` 的 scan |
| Heap AM | 行存储表 | `record_mgr` 的表页布局 |
| shared_buffers | 共享缓冲池 | `buffer_mgr` |
| nbtree | Lehman-Yao 风格 B-tree 访问方法 | `btree_mgr`（教学型、近似 B+ 树索引） |
| WAL | 预写日志 | **没有** |
| MVCC | 多版本并发控制 | **没有** |
| Lock Manager | 表 / 行级锁 | **没有** |
| BG Writer | 后台脏页刷盘 | **没有** |
| Checkpointer | 定期做检查点 | **没有** |
| pg_catalog | 系统表 | `catalog` |

**English**

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

## 9.3 Query Optimizer：为什么同一条 SQL 有多种跑法

**中文**

考虑这条 SQL：

```sql
SELECT * FROM orders o JOIN users u ON o.uid = u.id WHERE u.age > 18;
```

至少有三种执行方式：

1. **Nested loop join**：对 `orders` 每一行扫一遍 `users` 找匹配——O(n×m)
2. **Hash join**：先对 `users` 建哈希表，再用 `orders` 每行去查——O(n+m)
3. **Merge join**：两边按 join key 排序后归并——O(n log n + m log m)

哪种最快？取决于数据量、是否有索引、可用内存、统计信息。PostgreSQL 的 **Planner** 会：

1. 枚举可能的执行计划（join 顺序、join 方法、扫描方法）
2. 基于统计信息（`pg_class.reltuples` 等系统表存的行数估算）估算每个计划的代价
3. 选代价最小的那个

代价模型的基本单位是「顺序读一页的代价 = 1.0」，其他操作按比例换算。这就是为什么 `ANALYZE` 命令很重要——它刷新统计信息，否则优化器会基于过时数据做错误选择。可以用 `EXPLAIN` 看到一条 SQL 的执行计划，`EXPLAIN ANALYZE` 还会真的跑一遍并报告实际耗时。

**English**

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

## 9.4 Executor 火山模型：每个算子都是 open / next / close

**中文**

PostgreSQL 的执行器用**火山模型（Volcano model）**：每个算子（scan、filter、join、agg、sort……）都实现三个接口：

```
open()      ← 初始化
next()      ← 拉一行（或一批）上来
close()     ← 清理
```

数据是**拉取式（pull-based）**流动的——上层算子调用下层算子的 `next()`，下层被动产出一行。比如：

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

优点：实现简单、内存占用小（一次一行）、容易组合任意算子树。缺点：每行都要走一次函数调用链，对 CPU cache 不友好。一些现代引擎使用**向量化执行（vectorized execution）**摊薄开销；PostgreSQL 的标准执行器仍以 tuple-at-a-time 为主，LLVM JIT 会编译表达式和 tuple deforming 等热点，但不会把执行器变成向量化引擎。

**English**

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

## 9.5 MVCC：多版本让读不阻塞写

**中文**

我们的简易引擎里，一个 scan 拿到 page 之后就直接读，根本没有「事务」的概念。如果有人同时 UPDATE 这张表呢？轻则读到不一致的数据，重则读到半写入的脏行。

PostgreSQL 的解法是 **MVCC（Multi-Version Concurrency Control）**：UPDATE 创建新 tuple 版本并给旧版本标记 `xmax`，DELETE 标记被删除版本。可见性并不是「创建事务 ID ≤ 我的 ID < 删除事务 ID」这样的标量比较，而是结合事务快照（`xmin`、`xmax`、进行中的事务 ID 集合）、tuple 的 `xmin`/`xmax` 以及事务提交或回滚状态判断；隔离级别还决定快照何时取得。

效果：

- **读不阻塞写**：读事务读老版本，写事务写新版本，互不干扰
- **写不阻塞读**：写新版本时，老版本仍然可读
- 代价：表里会积累「死版本（dead tuples）」，需要 **VACUUM** 清理回收空间

MVCC 是 PostgreSQL 高并发能力的根基，也是它和 MySQL/InnoDB 的核心差异之一——MySQL 的 MVCC 在 undo log 里重建老版本，PostgreSQL 直接在表里留多版本。

**English**

In our small engine, a scan reads pages directly with no notion of a "transaction". What if someone UPDATEs the same table concurrently? You either read inconsistent data or read a half-written row.

PostgreSQL's answer is **MVCC (Multi-Version Concurrency Control)**: UPDATE creates a new tuple version and marks the old version with `xmax`; DELETE marks the deleted version. Visibility is not the scalar rule "creator ID ≤ my ID < deleter ID". It is decided from the transaction snapshot (`xmin`, `xmax`, in-progress transaction IDs), tuple `xmin`/`xmax`, and commit/abort status; isolation level also determines when snapshots are acquired.

Effects:

- **Reads don't block writes**: readers see old versions, writers create new ones, no interference
- **Writes don't block reads**: while a new version is being written, the old one stays readable
- Cost: dead tuples accumulate and need **VACUUM** to reclaim space

MVCC is the foundation of PostgreSQL's high-concurrency throughput, and a key difference from MySQL/InnoDB — MySQL rebuilds old versions from the undo log, while PostgreSQL keeps multiple versions right in the table.

---

## 9.6 WAL：先写日志，再改数据

**中文**

如果事务提交后立刻断电怎么办？内存里改了的数据没刷盘，但客户端已经收到 "COMMIT OK" 了——这就违背了**持久性（Durability）**。

PostgreSQL 的解法是 **WAL（Write-Ahead Log）**：所有修改在写到数据文件**之前**，先写到 WAL 日志文件里。规则只有一条：

> 一条日志记录被刷到磁盘之前，它对应的数据页不能被刷到磁盘。

这样恢复时只需重放 WAL：从最后一次 checkpoint 的位置开始，把日志里所有"已提交但未刷盘"的修改重新应用一遍。事务提交也变快了——只需 `fsync` WAL 文件（小、顺序写），不用 fsync 所有数据页（大、随机写）。

我们的简易引擎没有 WAL，进程崩了就什么都没了——这也是为什么 demo 跑完关掉进程，数据就消失的原因。

**English**

What if power dies right after a transaction commits? Modified pages in memory haven't been flushed, but the client already got "COMMIT OK" — that violates **Durability**.

PostgreSQL's answer is **WAL (Write-Ahead Log)**: every modification is written to a WAL log file **before** it is written to the data file. The rule is simple:

> A data page may not be flushed to disk until the WAL record describing its modification has been flushed.

Recovery then just replays the WAL: starting from the last checkpoint's position, reapply every "committed but not-yet-flushed" modification. Commit also gets faster — you only need to `fsync` the WAL file (small, sequential) instead of fsyncing every data page (large, random).

Our small engine has no WAL — kill the process and everything is gone. This is exactly why our `demo` "loses" its data as soon as the process exits.

---

## 9.7 Checkpoint：缩短恢复时间

**中文**

如果没有 checkpoint，崩溃恢复要重放从头到尾的 WAL——可能几个 GB。Checkpoint 做的事是：

1. 把所有脏页刷到数据文件
2. 在 WAL 里写一条 `CHECKPOINT` 记录，记下"此刻 WAL 的位置 LSN"
3. 之前 LSN 的 WAL 可以被回收复用

下次崩溃恢复时，只需从最近 checkpoint 的 LSN 开始重放，几秒钟到几分钟就够。

PostgreSQL 由 **Checkpointer** 进程定期触发（`checkpoint_timeout` 默认 5 分钟，或 `max_wal_size` WAL 量写满时触发）。配套的还有：

- **BG Writer**：平时慢慢刷脏页，让 checkpoint 不至于一次刷太多导致 I/O 抖动
- **WAL Writer**：提前把 WAL buffer 刷到磁盘，减轻提交时的 fsync 压力

**English**

Without checkpoints, crash recovery would replay the entire WAL from the beginning — potentially gigabytes. A checkpoint does:

1. Flush all dirty pages to data files
2. Write a `CHECKPOINT` record to the WAL noting the current WAL position (LSN)
3. WAL before that LSN can be recycled

Next crash recovery only replays from the most recent checkpoint's LSN — seconds to minutes.

In PostgreSQL the **Checkpointer** process triggers this periodically (`checkpoint_timeout` defaults to 5 min, or when `max_wal_size` of WAL has been written). Helpers:

- **BG Writer**: trickles dirty pages to disk so checkpoint doesn't cause an I/O spike
- **WAL Writer**: pre-flushes the WAL buffer to reduce fsync pressure at commit time

---

## 9.8 进程模型：一连接一进程

**中文**

PostgreSQL 的并发模型是 **process-per-connection**，不是 thread-per-connection：

```
客户端 A ─→ backend A ─┐
客户端 B ─→ backend B ─┼─→ 共享内存 (shared_buffers, WAL buffer, lock table)
客户端 C ─→ backend C ─┘         ↑
                          postmaster 主进程监听端口，
                          fork() 出每个 backend
```

postmaster 是主进程，监听端口、接收连接、`fork()` 一个 backend 子进程处理这个连接。backend 之间通过 **共享内存**（shared_buffers、WAL buffer、lock table）通信，不通过线程切换。

优点：进程崩了不会拖死整个数据库（postmaster 直接重启该 backend）；调试容易（一个 backend 一个 gdb）。缺点：进程比线程重，连接数多了内存开销大（所以有 PgBouncer 这种连接池中间件）。

**English**

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

## 9.9 简易引擎 → PostgreSQL 进化路线图

**中文**

| 我们的组件 | PostgreSQL 对应 | 进化点 |
|-----------|----------------|--------|
| `storage_mgr` | md smgr（底层文件抽象） | 接口更通用，支持多种 smgr 实现 |
| `buffer_mgr` | shared_buffers | 跨进程共享、时钟替换算法 |
| `record_mgr` | Heap AM | 支持多种表存储 AM（Heap、列存） |
| `btree_mgr` | nbtree | 并发 B-tree 访问方法、dedup 压缩 |
| `catalog` | pg_catalog 系统表 | 元数据完全用表存，可 SQL 查询 |
| `ddl_parser` | Parser + Analyzer | 完整 SQL 标准、视图重写 |
| `dml_parser` | Parser + Analyzer | 表达式、子查询、CTE |
| 没有 | Planner | 基于代价的优化器 |
| 没有 | Executor 火山模型 | 算子组合、JIT 编译 |
| 没有 | WAL | 崩溃恢复 |
| 没有 | MVCC | 多版本并发 |
| 没有 | Lock Manager | 表 / 行级锁 |
| 没有 | BG Writer / Checkpointer | 后台刷盘 |

一句话总结：**我们写了"骨架"，PostgreSQL 在骨架之上加了"大脑"（优化器）、"免疫系统"（MVCC + Lock）、"记忆"（WAL + Checkpoint）和"器官协作"（进程模型 + 后台守护）。**

**English**

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

## 9.10 推荐阅读

**中文**

1. **PostgreSQL 官方文档** —— `https://www.postgresql.org/docs/`，特别是 "Internals" 章节里的 `Overview of PostgreSQL Internals` 和 `System Catalogs`。最权威的一手资料。
2. **The Internals of PostgreSQL**（在线书，作者 Hironobu Suzuki）—— `https://www.interdb.jp/pg/`，图解 WAL、查询处理、进程模型，中文社区有译本，对初学者最友好。
3. **Database Internals**, Alex Petrov（O'Reilly, 2019）—— 从磁盘到事务到分布式，章节布局对学完本项目的学生特别友好。
4. **Architecture of a Database System**, Hellerstein, Stonebraker, Hamilton（CACM 2007 综述）—— 经典综述，把"数据库由哪些子系统组成"讲得最透彻，强烈推荐先读这篇再看其他。
5. **Readings in Database Systems**（Red Book, 5th ed.）—— 经典论文集，每个子领域都有奠基性论文。

**English**

1. **PostgreSQL Documentation** — `https://www.postgresql.org/docs/`, especially the "Internals" chapter (`Overview of PostgreSQL Internals`, `System Catalogs`). The authoritative source.
2. **The Internals of PostgreSQL** (online book, Hironobu Suzuki) — `https://www.interdb.jp/pg/`, illustrated guides to WAL, query processing, and the process model. Most beginner-friendly.
3. **Database Internals**, Alex Petrov (O'Reilly, 2019) — disk to transactions to distributed systems; the chapter layout is friendly for students who just finished a project like ours.
4. **Architecture of a Database System**, Hellerstein, Stonebraker, Hamilton (CACM 2007 survey) — the classic survey that explains "what subsystems make up a database" most clearly. Strongly recommended as the first thing to read.
5. **Readings in Database Systems** (Red Book, 5th ed.) — the canonical paper collection, with foundational papers for every sub-area.

---

## 9.11 思考题

1. **为什么 PostgreSQL 选择「每连接一个进程」而不是「每连接一个线程」？** 提示：从稳定性、调试、共享内存、操作系统的进程 / 线程调度差异几个角度想。这个选择带来了什么缺点？为什么 MySQL 选了相反的路？

2. **MVCC 让"读不阻塞写"，但代价是什么？** 如果一个长事务跑了 1 小时还没提交，期间别人 UPDATE 了大量数据，会发生什么？提示：死版本、VACUUM、表膨胀（bloat）。

3. **我们的简易引擎没有 Planner，所有 SQL 按"写出来的样子"执行。举一个具体例子说明这会很慢。** 提示：四表 join 的 join 顺序、过滤条件提前还是延后、有索引但优化器不知道该用哪个。

---

> 返回 [README](../README.md) 看项目总览。
