# 第8章 · 系统目录 Catalog

> 对应源文件：`catalog.c` / `catalog.h`
>
> 这是整个数据库的**元数据中心**——回答「`SELECT * FROM users` 里的 `users` 是谁、有哪些列、有没有索引」这一类问题。没有 Catalog，SQL 引擎就只能解析语法、却不知道表长什么样。

---

## 8.1 为什么需要这一层

**中文**

考虑一条最简单的 SQL：

```sql
SELECT * FROM users;
```

解析器能告诉你「这是在查一张叫 `users` 的表」，但接下来呢？执行器必须知道：

1. `users` 表存在吗？
2. 它有哪些列？每列的数据类型是什么？字符串长度是多少？
3. 哪些列构成主键？
4. 它有没有 B+ 树索引？索引文件叫什么名字？

Catalog 是 SQL 执行器使用的「表的表」，集中登记表名、schema 和索引信息，并在进程重启后恢复。需要注意：当前教学实现同时在每个表文件的 page 0 保存 schema，因此 Catalog 中的 schema 是一份重复副本，而不是唯一事实来源；实现尚未提供两份元数据的版本校验或一致性修复。

Catalog 的持久化直接调用 `storage_mgr`，使用独立 page file `catalog.bin`，而不是把目录本身实现为普通记录表。不过，当前 `openTable` 直接从表文件 page 0 读取 schema，并不查询 Catalog；`catalog.c` 也会调用 `record_mgr` 提供的 `createSchema`/`freeSchema`。因此这里是简化的存储选择，并没有完全消除模块依赖。

**English**

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

## 8.2 核心原理：独立 page file + 内存链表

**中文**

Catalog 在磁盘上是一个独立的 page file `catalog.bin`，布局如下：

```
┌──────────────────────────┬──────────────────────────────────────┐
│  Page 0                  │  Page 1, 2, ...                      │
│  [0..3] = entry count    │  串行化后的 CatalogEntry，跨页拼接    │
└──────────────────────────┴──────────────────────────────────────┘
```

- **Page 0 的前 4 字节**：存的是表项个数 `count`。
- **Page 1 起**：把所有 `CatalogEntry` 序列化后**背靠背**排在一起，必要时跨页。页边界不对齐没关系——`writeBlock` 一次写满 4096 字节，多出来的部分填零。

进程启动时 `initCatalog` 把整个文件读进内存，反序列化成一条**单向链表**；之后所有查询都直接走内存链表。任何 `register`/`drop` 操作完成后立即 `catalogFlush`，把整条链表写回磁盘。

**English**

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

## 8.3 关键数据结构

**中文**

```c
/* 一个表在目录中的一个条目 */
typedef struct CatalogEntry {
    char  *tableName;                /* 表名（堆上分配） */
    Schema *schema;                  /* 表的 schema（反序列化重建） */
    int    hasIndex;                 /* 0 = 无索引，1 = 有 B+ 树索引 */
    char  *indexName;                /* 例如 "users.idx"；无索引时为 NULL */
    struct CatalogEntry *next;       /* 单链表 */
} CatalogEntry;

/* 整个目录的句柄 */
typedef struct Catalog {
    CatalogEntry *head;              /* 链表头 */
    int           count;             /* 表的个数 */
    char          *pageFile;         /* "catalog.bin" */
} Catalog;
```

注意 `tableName` 和 `indexName` 都是堆上 `strdup` 出来的字符串——`CatalogEntry` 自己拥有它们，`shutdownCatalog` 时会逐个 `free`。`schema` 也是深拷贝：`catalogRegisterTable` 调 `createSchema` 复制调用方传入的 schema，这样调用方就可以安全释放自己的版本。

**English**

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

## 8.4 序列化：字节布局

**中文**

`CatalogEntry` 不是直接用 `fwrite` 写结构体——那样会有内存对齐、字节序、版本兼容的问题。我们采用**手写序列化**，把字段一个一个 `memcpy` 进字节缓冲区。一个 entry 的字节布局是：

```
┌──────────────┬─────────────┬──────────────┬──────────────┐
│ tableNameLen │ tableName   │ numAttr      │ keySize      │
│ (int, 4B)    │ (变长字节)   │ (int, 4B)    │ (int, 4B)    │
└──────────────┴─────────────┴──────────────┴──────────────┘
┌──────────────────────────────────────────────────────────┐
│  对每个 attr 重复：                                       │
│    attrNameLen(int) + attrName(bytes)                     │
│    + dataType(int) + typeLength(int)                      │
└──────────────────────────────────────────────────────────┘
┌────────────────────────┬──────────────┬──────────────────┐
│ keyAttrs[keySize]      │ hasIndex(int)│ indexNameLen(int)│
│ (keySize × 4B)         │              │                  │
└────────────────────────┴──────────────┴──────────────────┘
┌──────────────────┐
│ indexName(bytes) │  ← 只在 hasIndex 且 indexNameLen>0 时存在
└──────────────────┘
```

序列化和反序列化代码：

```c
static void
entrySerialize(char *buf, int *off, const CatalogEntry *e)
{
    int len;
    Schema *s = e->schema;

    len = (int) strlen(e->tableName);
    memcpy(buf + *off, &len, sizeof(int)); *off += sizeof(int);
    memcpy(buf + *off, e->tableName, len); *off += len;

    memcpy(buf + *off, &s->numAttr,  sizeof(int)); *off += sizeof(int);
    memcpy(buf + *off, &s->keySize,  sizeof(int)); *off += sizeof(int);

    for (int i = 0; i < s->numAttr; i++) {
        len = (int) strlen(s->attrNames[i]);
        memcpy(buf + *off, &len, sizeof(int)); *off += sizeof(int);
        memcpy(buf + *off, s->attrNames[i], len); *off += len;
        memcpy(buf + *off, &s->dataTypes[i],  sizeof(int)); *off += sizeof(int);
        memcpy(buf + *off, &s->typeLength[i], sizeof(int)); *off += sizeof(int);
    }
    for (int i = 0; i < s->keySize; i++) {
        memcpy(buf + *off, &s->keyAttrs[i], sizeof(int)); *off += sizeof(int);
    }

    memcpy(buf + *off, &e->hasIndex, sizeof(int)); *off += sizeof(int);

    if (e->hasIndex && e->indexName) {
        len = (int) strlen(e->indexName);
        memcpy(buf + *off, &len, sizeof(int)); *off += sizeof(int);
        memcpy(buf + *off, e->indexName, len); *off += len;
    } else {
        int zero = 0;
        memcpy(buf + *off, &zero, sizeof(int)); *off += sizeof(int);
    }
}
```

两个要点：

1. **长度前缀**：每个变长字符串前面都写一个 `int` 表示长度。这样反序列化时先读 4 字节知道要读多少字节，避免依赖 `\0` 终止符——也方便表名/列名里出现任意字符。
2. **`hasIndex=false` 时也写一个 `indexNameLen=0`**：让读端永远先读 4 字节决定要不要继续读 `indexName`，避免特殊分支。

反序列化 `entryDeserialize` 完全对称：先 `calloc` 一个 `CatalogEntry`，再按相同顺序 `memcpy` 字段，并对每个字符串 `malloc(len+1)` 后补 `\0`。

**English**

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

## 8.5 持久化：catalogFlush 与 initCatalog

**中文**

`catalogFlush` 做三件事：①遍历链表累加 `entrySize` 算出总字节数；②分配 `numPages = ceil(total / PAGE_SIZE)` 个页的缓冲区并把所有 entry 顺序序列化进去；③`ensureCapacity` 后逐页 `writeBlock`。

```c
static RC
catalogFlush(void)
{
    int total = sizeof(int);                 /* count */
    CatalogEntry *e = g_catalog.head;
    while (e) { total += entrySize(e); e = e->next; }
    int numPages = (total - 1) / PAGE_SIZE + 1;

    char *buf = (char *) calloc(numPages, PAGE_SIZE);
    int off = 0;
    memcpy(buf + off, &g_catalog.count, sizeof(int)); off += sizeof(int);
    e = g_catalog.head;
    while (e) { entrySerialize(buf, &off, e); e = e->next; }

    ensureCapacity(numPages, &g_catFH);
    for (int i = 0; i < numPages; i++)
        writeBlock(i, &g_catFH, buf + i * PAGE_SIZE);
    free(buf);
    return RC_OK;
}
```

`initCatalog` 则相反：若 `catalog.bin` 不存在就创建并写一个 `count=0` 的空目录；若存在就把所有页读进一个大缓冲区，先读出 `count`，再循环 `count` 次 `entryDeserialize` 重建链表。

```c
RC initCatalog(void)
{
    if (g_initialized) return RC_OK;
    initStorageManager();
    g_catalog.head = NULL;
    g_catalog.count = 0;
    g_catalog.pageFile = strdup("catalog.bin");

    if (openPageFile(g_catalog.pageFile, &g_catFH) != RC_OK) {
        createPageFile(g_catalog.pageFile);
        openPageFile(g_catalog.pageFile, &g_catFH);
        g_initialized = 1;
        catalogFlush();                 /* write count=0 */
        return RC_OK;
    }
    /* load existing entries from disk */
    char *buf = malloc(g_catFH.totalNumPages * PAGE_SIZE);
    for (int i = 0; i < g_catFH.totalNumPages; i++)
        readBlock(i, &g_catFH, buf + i * PAGE_SIZE);

    int off = 0;
    memcpy(&g_catalog.count, buf + off, sizeof(int)); off += sizeof(int);
    CatalogEntry *tail = NULL;
    for (int i = 0; i < g_catalog.count; i++) {
        CatalogEntry *e = entryDeserialize(buf, &off);
        if (tail == NULL) g_catalog.head = e;
        else tail->next = e;
        tail = e;
    }
    free(buf);
    g_initialized = 1;
    return RC_OK;
}
```

注意「整个文件一次性读进内存」的策略对小规模目录完全够用——表的数量通常只有几十张。如果目录规模到百万级，就应当改成按需读页或建索引。

**English**

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

## 8.6 与 DDL / DML 集成

**中文**

Catalog 真正发挥作用是在它被接入执行器时。`executeDDL` 在 `CREATE TABLE` 时先把表交给 `record_mgr` 的 `createTable`，建好索引文件后调用 `catalogRegisterTable`：

```c
case DDL_CREATE_TABLE: {
    rc = createTable(st->tableName, st->schema);
    if (rc != RC_OK) break;

    int hasIdx = 0;
    char idxName[256];
    if (st->primaryKeyAttr >= 0) {
        snprintf(idxName, sizeof(idxName), "%s.idx", st->tableName);
        rc = createBTree(idxName, kt, 0);
        hasIdx = 1;
    }
    /* 登记到 catalog；catalog 未初始化时静默忽略 */
    catalogRegisterTable(st->tableName, st->schema, hasIdx,
                         hasIdx ? idxName : NULL);
    break;
}
case DDL_DROP_TABLE: {
    rc = deleteTable(st->tableName);
    snprintf(idxName, sizeof(idxName), "%s.idx", st->tableName);
    deleteBTree(idxName);
    catalogDropTable(st->tableName);
    break;
}
```

`executeDML` 则在分派具体操作之前先 `catalogLookupTable`——找不到表直接报错，找到的 `CatalogEntry` 传给 `execSelect` / `execInsert` 等子函数：

```c
RC executeDML(DML_Statement *stmt)
{
    CatalogEntry *entry = catalogLookupTable(stmt->tableName);
    if (!entry) {
        fprintf(stderr, "[executor] table '%s' not found in catalog\n",
                stmt->tableName);
        return RC_IM_KEY_NOT_FOUND;
    }
    switch (stmt->type) {
        case DML_SELECT: return execSelect(stmt, entry);
        case DML_INSERT: return execInsert(stmt, entry);
        case DML_UPDATE: return execUpdate(stmt, entry);
        case DML_DELETE: return execDelete(stmt, entry);
    }
}
```

这种设计把「表存在吗、有哪些列」与「怎么执行这个 SQL」彻底解耦：执行器只接受一个已经验明正身的 `CatalogEntry`。

**English**

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

## 8.7 编译与运行

```bash
# 编译整个项目
make all

# 跑 DDL/DML 测试，间接验证 catalog（CREATE TABLE 会自动登记）
./build/test_ddl
./build/test_dml
```

如果想直接看 catalog 的内容，可以在自己的测试程序里调 `catalogPrint`：

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

`catalogPrint` 的输出大致长这样：

```
Catalog (2 tables):
  users  idx=users.idx  (id:int, name:str[32], age:int) PK{id}
  orders  idx=none  (oid:int, uid:int, total:float) PK{oid}
```

观察要点：①有主键的 `users` 自动登记了 `users.idx`；②`orders` 没有主键，所以 `idx=none`；③重启进程后 `initCatalog` 会重新加载这张表，目录不丢。

---

## 8.8 思考题

1. **为什么 Catalog 直接用 `storage_mgr` 存 `catalog.bin`，而不像普通表那样用 `record_mgr`？** 当前 `openTable` 并不查询 Catalog；请比较这种简化与「Catalog 是普通系统表」两种设计的启动和依赖关系。

2. **`catalogFlush` 每次都把整个链表重写一遍。如果一张数据库里有 10000 张表，每次 `CREATE TABLE` 都重写一遍目录文件，性能会有什么问题？如何改进？** 提示：增量写、WAL、缓存脏标记。

3. **当前 `catalogLookupTable` 是线性扫描链表。如果要支持百万级表，目录本身该长什么样？** 提示：把 catalog 当作一张普通的表，给它自己建一个 B+ 树索引。

---

> **下一章**：[第9章 · PostgreSQL全景与高阶组件](09-postgresql-overview.md)
