# A Simple Database Engine

> **English** ｜ [中文版](README.md)
>
> A hands-on project for IIT CS525 (Advanced Database Systems): build a layered
> database storage engine from scratch, one layer at a time.

This project breaks a database engine into modules that can be understood and
tested separately. The main data path is disk I/O → buffer pool → record
management; the B+ tree index is a sibling consumer of the lower page services,
while the DDL/DML executor composes records, indexes, and the catalog. It is not
a strict five-layer, one-way dependency stack.

---

## Architecture

```
┌─────────────────────────────────────────────┐
│  Layer 5  DDL Parser      ddl_parser.c       │  ← CREATE/DROP TABLE
├──────────────────┬──────────────────────────┤
│ Layer 3 Record   │  Layer 4 B+ Tree Index   │  ← Table CRUD / Index lookup
│  record_mgr.c    │   btree_mgr.c            │
├──────────────────┴──────────────────────────┤
│  Layer 2  Buffer Manager  buffer_mgr.c       │  ← FIFO/LRU (linked list + timestamps)
├─────────────────────────────────────────────┤
│  Layer 1  Storage Manager  storage_mgr.c     │  ← page file I/O
├─────────────────────────────────────────────┤
│           Disk (page files)                   │
└─────────────────────────────────────────────┘
```

Each layer's "why it exists", "how it works", and "how to test it" is
explained in its chapter.

---

## Reading Path

| Chapter | Topic | Source | Key question |
|---------|-------|--------|-------------|
| [Ch.1](docs/01-storage-manager.en.md) | Storage Manager | `storage_mgr.c` | What is the unit of disk I/O? |
| [Ch.2](docs/02-buffer-manager.en.md) | Buffer Manager | `buffer_mgr.c` | Why not read/write disk every time? |
| [Ch.3](docs/03-record-manager.en.md) | Record Manager | `record_mgr.c` | How to organize "tables" on top of pages? |
| [Ch.4](docs/04-btree-index.en.md) | B+ Tree Index | `btree_mgr.c` | How to go from O(n) to O(log n)? |
| [Ch.5](docs/05-ddl-parser.en.md) | DDL Parser | `ddl_parser.c` | How to let users create tables via SQL? |
| [Ch.6](docs/06-integration.en.md) | Integration | `demo_api.c` | Put it together: index vs. linear scan |
| [Ch.7](docs/07-dml-parser.en.md) | DML Parser | `dml_parser.c` | How to let users write SELECT/INSERT? |
| [Ch.8](docs/08-catalog.en.md) | System Catalog | `catalog.c` | How does the DB know what tables exist? |
| [Ch.9](docs/09-postgresql-overview.en.md) | PostgreSQL Overview | — | What does a real database look like? |

**Each chapter**: Why → Principle → Data structures → Key code → Build & run → Exercises

> 中文版文档请见 [README.md](README.md)。

---

## Quick Start

```bash
# Build all targets
make clean && make all

# Original regression tests (9 cases, includes 10000-row insert)
./build/test_assign3_1

# B+ tree unit tests (6 cases)
./build/test_btree

# DDL parser unit tests (5 cases)
./build/test_ddl

# DML + Catalog unit tests (6 cases)
./build/test_dml

# C API end-to-end demo: DDL → insert → index lookup → range scan
./build/demo_api

# SQL end-to-end demo: INSERT/SELECT/UPDATE/DELETE via SQL strings
./build/demo_sql
```

---

## File Map

All source and test files live under `src/`. Listed in chapter order; files
from the same chapter are grouped together.

### Core Source (by chapter)

| File | Role | Chapter |
|------|------|---------|
| `src/storage_mgr.c/h` | Page file read/write | Ch.1 |
| `src/buffer_mgr.c/h` | Buffer pool + FIFO/LRU replacement (linked list + timestamps) | Ch.2 |
| `src/buffer_mgr_stat.c/h` | Buffer pool statistics | Ch.2 |
| `src/record_mgr.c/h` | Table CRUD + linear scan | Ch.3 |
| `src/record_mgr_ex.h` | Record manager internals (legacy, no longer built) | Ch.3 |
| `src/expr.c/h` | Expression evaluation (WHERE condition tree) | Ch.3 |
| `src/rm_serializer.c` | Record/Schema/Value serialization | Ch.3 |
| `src/tables.h` | Core types (Value/RID/Record/Schema/RM_TableData) | Ch.3 |
| `src/btree_mgr.c/h` | B+ tree index (split/find/range scan) | Ch.4 |
| `src/ddl_parser.c/h` | DDL parser (CREATE/DROP TABLE) | Ch.5 |
| `src/demo_api.c` | C API end-to-end demo | Ch.6 |
| `src/dml_parser.c/h` | DML parser (SELECT/INSERT/UPDATE/DELETE) | Ch.7 |
| `src/query_executor.c/h` | DML executor (resolve columns → drive record_mgr+btree_mgr) | Ch.7 |
| `src/demo_sql.c` | SQL end-to-end demo | Ch.7 |
| `src/catalog.c/h` | System catalog (table registry, persisted to catalog.bin) | Ch.8 |

### Tests

| File | Role | Chapter |
|------|------|---------|
| `src/test_assign3_1.c` | Original course regression test (9 cases) | Ch.3 |
| `src/test_expr.c` | Expression evaluation test | Ch.3 |
| `src/test_btree.c` | B+ tree unit test (6 cases) | Ch.4 |
| `src/test_ddl.c` | DDL parser unit test (5 cases) | Ch.5 |
| `src/test_dml.c` | DML + Catalog unit test (6 cases) | Ch.7-8 |

### Infrastructure

| File | Role |
|------|------|
| `src/dberror.c/h` | Return codes + error messages (RC_OK/RC_IM_*/RC_RM_*) |
| `src/dt.h` | bool type definition |
| `src/test_helper.h` | Test macros (TEST_CHECK/ASSERT_TRUE/TEST_DONE) |
| `Makefile` | Build script (`make all` compiles everything under `src/`) |

---

## Tech Stack

- **Language**: C99 (`-std=c99 -g -Wall`)
- **Dependencies**: libc only (`stdio/stdlib/string/math`), no third-party libs
- **Platform**: Linux / macOS / WSL / MinGW
- **Build**: GNU Make

---

## License

Educational use, feel free to learn from it.
