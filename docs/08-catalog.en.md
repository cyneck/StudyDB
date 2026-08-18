# Chapter 8 · Catalog

> Corresponding source files: `catalog.c` / `catalog.h`
>
> This is the database's **metadata hub** — answering questions like "in `SELECT * FROM users`, who is `users`, what columns does it have, and does it have an index?" Without a Catalog, the SQL engine can only parse syntax but doesn't know what the tables look like.

---

## 8.1 Why we need this layer

Consider the simplest SQL:

```sql
SELECT * FROM users;
```

The parser tells you "this is querying a table called `users`", but then what? The executor must know:

1. Does `users` exist?
2. What columns does it have? Their types? String lengths?
3. Which columns form the primary key?
4. Does it have a B+ tree index? What is the index file name?

The Catalog is the SQL executor's persistent "table of tables", registering table names, schemas, and index metadata. In this teaching implementation, however, each table file also stores its schema on page 0. The catalog schema is therefore a duplicate rather than the single source of truth, and no version check or reconciliation mechanism exists yet.

Catalog persistence talks directly to `storage_mgr` and uses a standalone page file (`catalog.bin`) instead of representing the catalog as a normal record table. However, `openTable` reads its schema directly from table page 0 rather than consulting the Catalog, and `catalog.c` uses `createSchema`/`freeSchema` from `record_mgr`. This is a simplified storage choice, not complete dependency elimination.

---

## 8.2 Core principle: standalone page file + in-memory linked list

The Catalog is a standalone page file `catalog.bin` laid out as:

```
┌──────────────────────────┬──────────────────────────────────────┐
│  Page 0                  │  Page 1, 2, ...                      │
│  [0..3] = entry count    │  serialised CatalogEntries, packed    │
└──────────────────────────┴──────────────────────────────────────┘
```

- **First 4 bytes of Page 0**: the entry count.
- **Page 1+**: all `CatalogEntry` records serialised back-to-back, spilling across page boundaries as needed. Misalignment at page edges is fine — `writeBlock` always writes a full 4096-byte page, padding the tail with zeros.

At startup, `initCatalog` reads the whole file into memory and deserialises it into a **singly linked list**. All subsequent lookups go through the in-memory list. Any `register`/`drop` operation immediately calls `catalogFlush` to persist the whole list back to disk.

---

## 8.3 Key data structures

```c
/* One table entry in the catalog. */
typedef struct CatalogEntry {
    char  *tableName;
    Schema *schema;
    int    hasIndex;                 /* 0 = no index, 1 = has a B+ tree index */
    char  *indexName;                /* e.g. "users.idx", or NULL */
    struct CatalogEntry *next;
} CatalogEntry;

/* The catalog handle. */
typedef struct Catalog {
    CatalogEntry *head;
    int           count;
    char          *pageFile;         /* "catalog.bin" */
} Catalog;
```

Note that `tableName` and `indexName` are heap-allocated via `strdup` — the `CatalogEntry` owns them, and `shutdownCatalog` frees each one. The `schema` is also deep-copied: `catalogRegisterTable` calls `createSchema` to duplicate the caller's schema, so the caller can safely free their own copy.

---

## 8.4 Serialisation: byte layout

`CatalogEntry` is not written with `fwrite` on the raw struct — that would bring alignment, endianness, and versioning problems. Instead we hand-serialise fields one by one with `memcpy` into a byte buffer. The layout is:

```
┌──────────────┬─────────────┬──────────────┬──────────────┐
│ tableNameLen │ tableName   │ numAttr      │ keySize      │
│ (int, 4B)    │ (var bytes) │ (int, 4B)    │ (int, 4B)    │
└──────────────┴─────────────┴──────────────┴──────────────┘
┌──────────────────────────────────────────────────────────┐
│  for each attr:                                          │
│    attrNameLen(int) + attrName(bytes)                    │
│    + dataType(int) + typeLength(int)                     │
└──────────────────────────────────────────────────────────┘
┌────────────────────────┬──────────────┬──────────────────┐
│ keyAttrs[keySize]      │ hasIndex(int)│ indexNameLen(int)│
│ (keySize × 4B)         │              │                  │
└────────────────────────┴──────────────┴──────────────────┘
┌──────────────────┐
│ indexName(bytes) │  ← present only if hasIndex && indexNameLen>0
└──────────────────┘
```

Serialisation code:

```c
static void
entrySerialize(char *buf, int *off, const CatalogEntry *e)
{
    /* ... see source above ... */
}
```

Two key points:

1. **Length prefixes**: every variable-length string is preceded by an `int` length. Deserialisation reads 4 bytes first to know how many to follow, avoiding dependence on `\0` terminators — and allowing arbitrary bytes in names.
2. **Even when `hasIndex=false`, we still write `indexNameLen=0`**: the reader always reads 4 bytes to decide whether to continue, avoiding special-casing.

`entryDeserialize` is fully symmetric: `calloc` a `CatalogEntry`, `memcpy` fields in the same order, and `malloc(len+1)` each string with a trailing `\0`.

---

## 8.5 Persistence: catalogFlush and initCatalog

`catalogFlush` does three things: ①walks the list summing `entrySize` to compute total bytes; ②allocates `numPages = ceil(total / PAGE_SIZE)` pages of buffer and serialises all entries back-to-back; ③`ensureCapacity`s then `writeBlock`s each page.

```c
static RC
catalogFlush(void)
{
    /* ... see source above ... */
}
```

`initCatalog` is the inverse: if `catalog.bin` doesn't exist, create it and write a `count=0` empty catalog; otherwise read all pages into one big buffer, read out `count`, then loop `count` times calling `entryDeserialize` to rebuild the linked list.

Note the "read the whole file into memory" strategy is fine for a small catalog — table counts are typically in the tens. At millions of tables you'd want on-demand page reads or a B+ tree over the catalog itself.

---

## 8.6 Integration with DDL / DML

The Catalog earns its keep once it's wired into the executor. On `CREATE TABLE`, `executeDDL` first asks `record_mgr`'s `createTable` to build the table, builds the index file if there is a primary key, and finally calls `catalogRegisterTable`:

```c
case DDL_CREATE_TABLE: {
    /* ... see source above ... */
    catalogRegisterTable(st->tableName, st->schema, hasIdx,
                         hasIdx ? idxName : NULL);
    break;
}
case DDL_DROP_TABLE: {
    /* ... */
    catalogDropTable(st->tableName);
    break;
}
```

`executeDML` calls `catalogLookupTable` before dispatching — if the table isn't found, it errors out immediately; otherwise the returned `CatalogEntry` is passed down to `execSelect` / `execInsert` / etc.:

```c
RC executeDML(DML_Statement *stmt)
{
    /* ... see source above ... */
}
```

This design fully decouples "does the table exist and what are its columns" from "how do we execute this SQL": the executor only ever receives an already-validated `CatalogEntry`.

---

## 8.7 Build and run

```bash
# 编译整个项目
make all

# 跑 DDL/DML 测试，间接验证 catalog（CREATE TABLE 会自动登记）
./build/test_ddl
./build/test_dml
```

If you want to inspect the catalog directly, call `catalogPrint` from your own test program:

```c
#include "catalog.h"
int main(void) {
    initCatalog();           /* 启动时加载 catalog.bin */
    /* ... 执行若干 CREATE TABLE ... */
    catalogPrint();          /* 打印当前所有表 */
    shutdownCatalog();       /* 关闭时自动 flush 到 catalog.bin */
    return 0;
}
```

`catalogPrint` output looks roughly like:

```
Catalog (2 tables):
  users  idx=users.idx  (id:int, name:str[32], age:int) PK{id}
  orders  idx=none  (oid:int, uid:int, total:float) PK{oid}
```

Things to notice: ①`users`, which has a primary key, automatically registers `users.idx`; ②`orders` has no primary key, so `idx=none`; ③after restarting the process, `initCatalog` reloads the same list — the catalog isn't lost.

---

## 8.8 Exercises

1. **Why does the Catalog use `storage_mgr` directly instead of representing itself as a normal table?** `openTable` currently does not consult the Catalog; compare this simplification with a catalog implemented as ordinary system tables.

2. **`catalogFlush` rewrites the whole list every time. If a database has 10,000 tables, rewriting the catalog file on every `CREATE TABLE` causes what performance problems? How would you improve it?** Hint: incremental writes, WAL, dirty-buffer flags.

3. **`catalogLookupTable` is currently a linear scan of the linked list. To support millions of tables, what should the catalog itself look like?** Hint: treat the catalog as an ordinary table and build a B+ tree index over it.

---

> **Next chapter**: [Chapter 9 · PostgreSQL Panorama](09-postgresql-overview.en.md)
