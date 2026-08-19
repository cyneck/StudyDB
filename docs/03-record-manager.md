# 第3章 · 记录管理器 Record Manager

> 对应源文件：`record_mgr.c` / `record_mgr.h` / `tables.h`
>
> 第1章把磁盘抽象成「页的序列」，第2章把页缓存在内存里。但用户视角里没有「页」——他们看到的是「表、行、列」。**记录管理器（Record Manager）**就是这两层之上的翻译官：把定长页组织成「带 schema 的记录集合」。

---

## 3.1 为什么需要这一层

**中文**

存储管理器只懂「第几页」，缓冲池只懂「pin / unpin 一页」。但 SQL 层的用户视角是：

- 一张表有名字、若干列、每列有类型
- 行可以增删改查
- 查询时按条件过滤，返回的是**行**而不是字节

这中间有巨大的鸿沟：**定长 4096 字节的页 vs. 变长记录的集合**。记录管理器就是来填这个坑的。它做三件事：

1. **描述结构**——用 `Schema` 描述「这张表有几列、各列叫什么、什么类型」
2. **定位记录**——用 `RID = (page, slot)` 唯一标识一行
3. **CRUD + 扫描**——在页之上提供 `insertRecord` / `deleteRecord` / `updateRecord` / `getRecord` / `startScan` 等表语义接口

**English**

The storage manager only knows "which page"; the buffer pool only knows "pin / unpin a page". But the SQL-level user view is quite different:

- A table has a name, a set of columns, each with a type
- Rows can be inserted, deleted, updated, queried
- Queries filter rows by condition and return **rows**, not bytes

There's a big gap here: **fixed 4096-byte pages vs. a collection of variable-length records**. The record manager fills that gap. It does three things:

1. **Describe structure** — use `Schema` to describe "how many columns, what they're called, what types"
2. **Locate records** — use `RID = (page, slot)` to uniquely identify a row
3. **CRUD + scan** — provide table-semantics APIs like `insertRecord` / `deleteRecord` / `updateRecord` / `getRecord` / `startScan` on top of pages

---

## 3.2 核心原理：表文件的页组织

**中文**

一个表文件在磁盘上长这样：

```
┌──────────┬──────────────┬──────────────┬─────┬──────────────┐
│  page 0  │   page 1     │   page 2     │ ... │   page N     │
│ Schema + │  数据页 1    │  数据页 2    │     │  数据页 N    │
│ numTuples│              │              │     │              │
└──────────┴──────────────┴──────────────┴─────┴──────────────┘
```

- **page 0** 存「Schema 元信息 + 元组数」：序列化后的 `Schema` + 当前元组数 `numTuples`（见 3.4.2）。
- **page 1..N** 是**数据页**，每页由固定大小的槽组成：

```
┌─────────────┬─────────────┬─────────────┬─────┬─────────────┐
│   slot 0    │   slot 1    │   slot 2    │ ... │   slot K    │
├─────────────┴─────────────┴─────────────┴─────┴─────────────┤
│ 每个槽 = [ marker(1B) | 记录字节(recordSize) ]，共 recordSize+2 字节   │
└─────────────────────────────────────────────────────────────┘
```

槽的**标记字节（marker）**决定槽的状态：

- `'+'`：已占用（槽里有一条有效记录）
- `'-'`：墓碑——记录被删了，字节还在但逻辑上已删（见 3.4.4）
- `'\0'`：从未使用过（新页由 `createPageFile` 全零填充）

**RID → 字节偏移公式**：给定 `RID = (page, slot)`，记录在所在页内的字节偏移是

```
offset = slot × (recordSize + 2) + 1
         ↑ 跳过 marker 字节
```

每个槽都固定占 `recordSize + 2` 字节（1 字节标记 + 定长记录），所以删记录不会让任何槽变小、RID 永远指向同一个槽——这是「标记 + 定长槽」模型的核心权衡。

**English**

A table file on disk looks like this:

```
┌──────────┬──────────────┬──────────────┬─────┬──────────────┐
│  page 0  │   page 1     │   page 2     │ ... │   page N     │
│ Schema + │  data pg 1   │  data pg 2   │     │  data pg N   │
│ numTuples│              │              │     │              │
└──────────┴──────────────┴──────────────┴─────┴──────────────┘
```

- **page 0** stores the schema metadata + the current tuple count `numTuples` (see 3.4.2).
- **page 1..N** are **data pages**, each made of fixed-size slots:

```
┌─────────────┬─────────────┬─────────────┬─────┬─────────────┐
│   slot 0    │   slot 1    │   slot 2    │ ... │   slot K    │
├─────────────┴─────────────┴─────────────┴─────┴─────────────┤
│ each slot = [ marker(1B) | record bytes(recordSize) ], total recordSize+2 bytes │
└─────────────────────────────────────────────────────────────┘
```

A slot's **marker byte** encodes its state:

- `'+'`: occupied — a live record lives here
- `'-'`: tombstone — the record was deleted; bytes remain but it is logically gone (see 3.4.4)
- `'\0'`: never used (new pages are zero-filled by `createPageFile`)

**RID → byte offset formula**: given `RID = (page, slot)`, the record's in-page byte offset is

```
offset = slot × (recordSize + 2) + 1
         ↑ skip the marker byte
```

Every slot is a fixed `recordSize + 2` bytes (1 marker + fixed-length record), so deleting a record never resizes a slot and a RID always points at the same slot — the key trade-off of the marker + fixed-slot model.

---

## 3.3 关键数据结构

**中文**

四个核心类型（`tables.h`）：

```c
typedef enum DataType { DT_INT, DT_STRING, DT_FLOAT, DT_BOOL } DataType;

typedef struct RID { int page; int slot; } RID;             // 记录的唯一 id

typedef struct Record { RID id; char *data; } Record;       // 一条记录

typedef struct Schema {
    int numAttr;          // 列数
    char **attrNames;     // 各列名称
    DataType *dataTypes;  // 各列类型
    int *typeLength;      // DT_STRING 的长度（其他类型忽略）
    int *keyAttrs;        // 主键列下标
    int keySize;          // 主键个数
} Schema;
```

`Schema` 是一张表的「形状说明书」：要建表就得先有它。`typeLength` 只对 `DT_STRING` 有意义（定长字符串），其他类型的大小由 `dataTypes` 决定——`DT_INT` 占 4 字节、`DT_FLOAT` 占 4 字节、`DT_BOOL` 占 1 字节。

扩展类型（本实现不再用 `record_mgr_ex.h`，改成两个更简化的做法）：

- 扫描状态结构是 `record_mgr.c` 内部的 `RM_ScanInfo`：记录当前页/槽、扫描条件，以及**扫描开始时的表总页数** `totalPages`（扫描用它当上界，见 3.4.5）。
- 元组数不再塞进 `mgmtData`，而是直接成为 `RM_TableData` 的字段 `numTuples`。

`RM_TableData` 是用户拿到表的句柄：

```c
typedef struct RM_TableData {
    char *name;       // 表名
    Schema *schema;   // 表结构
    void *mgmtData;   // 装着缓冲池句柄（BM_BufferPool*）
    int numTuples;    // 当前元组数：closeTable 写回 page 0，openTable 恢复
} RM_TableData;
```

`mgmtData` 用 `void*` 跟第1章的 `SM_FileHandle.mgmtInfo` 是一个套路——信息隐藏，让上层只关心表语义，不暴露内部缓冲池结构。

**English**

Four core types (`tables.h`):

```c
typedef enum DataType { DT_INT, DT_STRING, DT_FLOAT, DT_BOOL } DataType;

typedef struct RID { int page; int slot; } RID;             // unique record id

typedef struct Record { RID id; char *data; } Record;       // one record

typedef struct Schema {
    int numAttr;          // number of columns
    char **attrNames;     // column names
    DataType *dataTypes;  // column types
    int *typeLength;      // length for DT_STRING (ignored otherwise)
    int *keyAttrs;        // primary-key column indices
    int keySize;          // number of key columns
} Schema;
```

`Schema` is the "shape spec" of a table — you must build one before creating a table. `typeLength` only matters for `DT_STRING` (fixed-length strings); other types' sizes are determined by `dataTypes` — `DT_INT` is 4 bytes, `DT_FLOAT` is 4 bytes, `DT_BOOL` is 1 byte.

Extension types (this implementation drops `record_mgr_ex.h` in favor of two simpler choices):

- The scan state is `RM_ScanInfo`, internal to `record_mgr.c`: it holds the current page/slot, the scan condition, and the table's **total page count at scan start** `totalPages` (used as the scan bound, see 3.4.5).
- The tuple count lives directly on `RM_TableData` as `numTuples`, not inside `mgmtData`.

`RM_TableData` is the user-facing table handle:

```c
typedef struct RM_TableData {
    char *name;       // table name
    Schema *schema;   // table schema
    void *mgmtData;   // holds the buffer pool handle (BM_BufferPool*)
    int numTuples;    // tuple count: persisted to page 0 by closeTable, restored by openTable
} RM_TableData;
```

`mgmtData` is `void*` for the same information-hiding reason as `SM_FileHandle.mgmtInfo` in Chapter 1 — upper layers see only table semantics, not the internal buffer-pool structure.

---

## 3.4 关键代码逐行讲

### 3.4.1 一条记录有多大：getRecordSize

**中文**

要往页里塞记录，必须先知道一条记录占多少字节。`getRecordSize` 按 Schema 累加各列大小：

```c
int getRecordSize(Schema *schema) {
    int i, size = 0;
    for (i = 0; i < schema->numAttr; i++) {
        switch (schema->dataTypes[i]) {
            case DT_INT:    size += sizeof(int);              break;
            case DT_STRING: size += schema->typeLength[i];    break;
            case DT_FLOAT:  size += sizeof(float);            break;
            case DT_BOOL:   size += sizeof(bool);            break;
        }
    }
    return size;
}
```

注意：字符串是**定长**的（长度由 `typeLength[i]` 决定），不是 C 字符串那种以 `\0` 结尾的变长。这样每条记录长度都相同，第 N 个槽的偏移就能用 `slot × (recordSize + 2)` 直接算出来（+2 是槽的标记字节）——这是「定长记录 + 标记槽」模型的关键权衡。

**English**

To pack records into pages we must first know a record's byte size. `getRecordSize` sums column sizes per the Schema:

```c
int getRecordSize(Schema *schema) {
    int i, size = 0;
    for (i = 0; i < schema->numAttr; i++) {
        switch (schema->dataTypes[i]) {
            case DT_INT:    size += sizeof(int);              break;
            case DT_STRING: size += schema->typeLength[i];    break;
            case DT_FLOAT:  size += sizeof(float);            break;
            case DT_BOOL:   size += sizeof(bool);            break;
        }
    }
    return size;
}
```

Note: strings are **fixed-length** (length given by `typeLength[i]`), not C-string-style variable-with-`\0`. This keeps every record the same length, so the Nth slot's offset is simply `slot × (recordSize + 2)` (the +2 being the slot's marker byte). This fixed-length-record + marker-slot trade-off is what makes slot-based addressing work.

---

### 3.4.2 Schema 持久化：writeTableSchema / openTable

**中文**

表关掉再打开时，`Schema` 必须能从磁盘读回。本实现的 `writeTableSchema` 把 Schema + 元组数序列化到 page 0（schema 太大可能溢出到 page 1+，所以 `numPagesOfSchema` 也一并记下）。注意它**直接通过存储管理器写文件**（`writeBlock`），不经过缓冲池——建表时表还没打开，没有缓冲池可用：

```c
static RC writeTableSchema(SM_FileHandle *fh, Schema *schema, int numTuples) {
    int i;

    int sizeSchema = sizeof(int) * (4 + schema->numAttr * 3 + schema->keySize)
                     + sizeof(DataType) * schema->numAttr;
    for (i = 0; i < schema->numAttr; i++)
        sizeSchema += strlen(schema->attrNames[i]) + 1;

    int numPagesOfSchema = (sizeSchema - 1) / PAGE_SIZE + 1;
    char *buffer = (char *) calloc(numPagesOfSchema, PAGE_SIZE);
    if (buffer == NULL) return RC_RM_MEM_ALLOC_FAILED;

    int attrNameOffset = sizeSchema;   // 字符串区从元数据区之后开始
    int offset = 0;

    memcpy(buffer + offset, &numPagesOfSchema, sizeof(int)); offset += sizeof(int);
    memcpy(buffer + offset, &numTuples,         sizeof(int)); offset += sizeof(int);
    memcpy(buffer + offset, &schema->numAttr,   sizeof(int)); offset += sizeof(int);
    memcpy(buffer + offset, &schema->keySize,   sizeof(int)); offset += sizeof(int);

    for (i = 0; i < schema->numAttr; i++) {
        int nameLen = strlen(schema->attrNames[i]) + 1;
        memcpy(buffer + offset, &attrNameOffset, sizeof(int));    offset += sizeof(int);
        memcpy(buffer + offset, &nameLen,        sizeof(int));    offset += sizeof(int);
        memcpy(buffer + offset, &schema->dataTypes[i],  sizeof(DataType)); offset += sizeof(DataType);
        memcpy(buffer + offset, &schema->typeLength[i], sizeof(int));       offset += sizeof(int);
        memcpy(buffer + attrNameOffset, schema->attrNames[i], nameLen);
        attrNameOffset += nameLen;
    }
    for (i = 0; i < schema->keySize; i++) {
        memcpy(buffer + offset, &schema->keyAttrs[i], sizeof(int)); offset += sizeof(int);
    }

    for (i = 0; i < numPagesOfSchema; i++)
        if (writeBlock(i, fh, buffer + i * PAGE_SIZE) != RC_OK) {
            free(buffer);
            return RC_WRITE_FAILED;
        }
    free(buffer);
    return RC_OK;
}
```

page 0 的字节布局：

```
┌──────────────┬──────────┬─────────┬─────────┬───────────────┬─────────────┐
│numPagesOfSche│ numTuples│ numAttr │ keySize │ 每列元信息×N  │ keyAttrs×K  │ ... │ attrNames 区 │
│   (int)      │  (int)   │ (int)   │ (int)   │ offset,slen,  │  (int×K)    │     │ (字符串拼接) │
│              │          │         │         │ dataType,typLen│            │     │              │
└──────────────┴──────────┴─────────┴─────────┴───────────────┴─────────────┘
```

`createTable` 调它（numTuples 传 0）把 schema 写进新文件。`openTable` 是镜像操作：先把 page 0 读进缓冲池，读出 `numPagesOfSchema` 和 `numTuples`，把那几页读入 buffer，按相反顺序反序列化回 `Schema*`（用 `createSchema` 重建，属性名字符串重新 malloc），并把 `numTuples` 赋给 `rel->numTuples`。`closeTable` 则在关表前把内存里的 `numTuples` 写回 page 0 的 `[4..7]`——这样「close → reopen」之后元组数能存活。

**English**

When a table is closed and reopened, its Schema must be readable from disk. Our `writeTableSchema` serializes the Schema + tuple count into page 0 (large schemas may spill onto page 1+, so `numPagesOfSchema` is stored too). Note it writes **directly through the storage manager** (`writeBlock`) rather than through the buffer pool — at create time the table is not open, so no pool exists yet:

```c
// (same code as above)
```

Page 0 byte layout:

```
┌──────────────┬──────────┬─────────┬─────────┬───────────────┬─────────────┐
│numPagesOfSche│ numTuples│ numAttr │ keySize │ per-attr meta×N│ keyAttrs×K  │ ... │ attrNames area │
│   (int)      │  (int)   │ (int)   │ (int)   │ offset,slen,  │  (int×K)    │     │ (concatenated) │
│              │          │         │         │ dataType,typLen│            │     │              │
└──────────────┴──────────┴─────────┴─────────┴───────────────┴─────────────┘
```

`createTable` calls it (numTuples = 0) to write the schema into the fresh file. `openTable` is the mirror: it pins page 0, reads `numPagesOfSchema` and `numTuples`, loads those pages into a buffer, deserializes back into a `Schema*` (rebuilt via `createSchema`, with freshly malloc'd attribute-name strings), and stores `numTuples` into `rel->numTuples`. `closeTable` writes the in-memory `numTuples` back to page 0 `[4..7]` before shutting the pool down — so the count survives a close → reopen round trip.

---

### 3.4.3 插入记录：insertRecord

**中文**

`insertRecord` 的流程：**从第 1 页开始逐页找空 → 数每页已占用槽 → 有空就插 → 回填 RID**。不需要在 page 0 记「数据页数」——`pinPage` 碰到不存在的页会自动把文件扩一页（见第 2 章），循环到表末尾时自然就开了新页：

```c
RC insertRecord(RM_TableData *rel, Record *record) {
    int recordSize = getRecordSize(rel->schema);
    int numSlots = getRecordsPerPage(rel->schema);

    int curPageNum = 1;                 // 数据从 page 1 开始
    bool foundPage = false;

    while (!foundPage) {
        BM_PageHandle pageHandle;
        if (pinPage(rel->mgmtData, &pageHandle, curPageNum) != RC_OK)
            return RC_RM_BUFFER_PIN_FAILED;

        // 找页内第一个空槽（标记 != '+'：未用 '\0' 或墓碑 '-'）
        int slotNum = -1;
        for (int i = 0; i < numSlots; i++) {
            int offset = i * (recordSize + 2);
            if (pageHandle.data[offset] != '+') { slotNum = i; break; }
        }

        if (slotNum >= 0) {                                     // 这页还有空位
            int offset = slotNum * (recordSize + 2);
            memcpy(pageHandle.data + offset + 1, record->data, recordSize);
            pageHandle.data[offset] = '+';                      // 槽标记置为占用
            record->id.page = curPageNum;
            record->id.slot = slotNum;

            if (markDirty(rel->mgmtData, &pageHandle) != RC_OK)
                return RC_RM_MARK_DIRTY_FAILED;
            rel->numTuples++;                                   // 内存里的元组数 +1
            if (unpinPage(rel->mgmtData, &pageHandle) != RC_OK)
                return RC_RM_BUFFER_UNPIN_FAILED;
            return RC_OK;
        }
        unpinPage(rel->mgmtData, &pageHandle);                  // 这页满了，看下一页
        curPageNum++;
    }
    return RC_OK;
}
```

关键点：

- **页满检测靠找空槽**：每页扫一遍 `numSlots` 个槽，遇到第一个标记不是 `'+'` 的槽就是空位。代价是 O(每页槽数)，但实现简单直观。
- **插入位置 = 第一个空槽**：插入时复用页内第一个空槽——无论是从未用过的 `'\0'` 槽还是删除留下的墓碑 `'-'` 槽。这样既不会覆盖仍占用的记录，也能回收墓碑空间（见 3.4.4 的讨论）。
- **文件按需增长**：`pinPage` 遇到 `curPageNum` 超出文件页数会自动 `appendEmptyBlock` 扩一页，所以循环必然终止，无需手动维护「数据页数」。
- **RID 即返回值**：插入成功后 `record->id` 被填上 `(page, slot)`，调用方拿到就能用于后续 `getRecord` / `deleteRecord`。
- **`markDirty` + `unpin`**：只标记脏页、释放 pin，真正的刷盘交给缓冲池（关表时 `forceFlushPool` 统一写回），不在每次插入时刷盘。

**English**

`insertRecord` flow: **walk pages from page 1 → find the first marker that is not `'+'` → insert → backfill RID**. This reuses both never-used (`'\0'`) slots and delete tombstones (`'-'`). No data-page count is kept on page 0 — `pinPage` auto-grows the file when asked for a page that does not exist yet (chapter 2), so the loop naturally opens a new page when it reaches the end of the table:

```c
// (same code as above)
```

Key points:

- **Page-full check by marker scan**: each page's `numSlots` markers are scanned until the first marker other than `'+'` is found. This is O(slots per page), but it is simple and reuses holes.
- **Insert position = first free slot**: tombstones left by deletes are reused before the file grows.
- **On-demand file growth**: `pinPage` calls `appendEmptyBlock` when `curPageNum` is past the end of the file, so the loop is guaranteed to terminate without manually tracking a "number of data pages".
- **RID is the return value**: on success, `record->id` is filled with `(page, slot)`, which the caller can pass to `getRecord` / `deleteRecord`.
- **`markDirty` + `unpin`**: only marks the page dirty and releases the pin; the actual flush is left to the buffer pool (`forceFlushPool` at close), not per-insert.

---

### 3.4.4 删除与墓碑：deleteRecord

**中文**

直接把记录从页里抹掉会破坏 `slot × (recordSize + 2)` 的偏移公式（后面的记录会错位）。本实现用最朴素的**墓碑（tombstone）**：不删记录字节，只把该槽的**标记字节**写成 `'-'`，物理上不删，逻辑上视为已删：

```c
RC deleteRecord(RM_TableData *rel, RID id) {
    int pageSize = PAGE_SIZE;
    int recordSize = getRecordSize(rel->schema);
    int numSlots = getRecordsPerPage(rel->schema);

    BM_PageHandle pageHandle;
    if (pinPage(rel->mgmtData, &pageHandle, id.page) != RC_OK)
        return RC_RM_BUFFER_PIN_FAILED;

    int offset = id.slot * (recordSize + 2);
    char marker = pageHandle.data[offset];
    if (marker != '+') {                        // 槽本来就是空的/已删
        unpinPage(rel->mgmtData, &pageHandle);  // 先释放 pin 再报错
        return RC_RM_INVALID_RID;
    }

    pageHandle.data[offset] = '-';              // 打墓碑
    if (markDirty(rel->mgmtData, &pageHandle) != RC_OK)
        return RC_RM_MARK_DIRTY_FAILED;
    rel->numTuples--;                           // 内存里的元组数 -1
    if (unpinPage(rel->mgmtData, &pageHandle) != RC_OK)
        return RC_RM_BUFFER_UNPIN_FAILED;
    return RC_OK;
}
```

`getRecord` 同样先看标记字节：不是 `'+'` 就返回 `RC_RM_INVALID_RID`（524），访问已删/空槽是非法操作：

```c
char marker = pageHandle.data[offset];
if (marker != '+') {
    unpinPage(rel->mgmtData, &pageHandle);      // 先释放 pin 再报错
    return RC_RM_INVALID_RID;
}
```

扫描（3.4.5）遇到 `'-'` 槽也直接跳过。这是最简单的「软删除」——

- **优点**：实现极简，不影响 RID 寻址，`RID = (page, slot)` 在删除前后含义一致；墓碑只占 1 个标记字节，几乎不浪费空间
- **缺点**：空间不完全回收——文件不会自动缩小，被删空的数据页留在文件末尾；墓碑槽虽会被插入复用（见 3.4.3），但页内不会压缩，反复增删仍可能让文件「虚胖」

**English**

Directly erasing a record from a page would break the `slot × (recordSize + 2)` offset formula (subsequent records would shift). This implementation uses the simplest form of **tombstone**: instead of removing the record bytes, it flips the slot's **marker byte** to `'-'`. Physically nothing is removed; logically the record is gone:

```c
// (same code as above)
```

`getRecord` checks the marker byte first; anything other than `'+'` returns `RC_RM_INVALID_RID` (524) — reading a deleted or empty slot is an illegal operation:

```c
// (same code as above)
```

Scans (3.4.5) skip `'-'` slots too. This is the simplest "soft delete":

- **Pro**: dead-simple, doesn't disturb RID addressing — `RID = (page, slot)` means the same thing before and after a delete; a tombstone costs only 1 marker byte.
- **Con**: tombstone slots are reused, but the file never shrinks and completely empty tail pages are not truncated, so repeated growth can still leave the file larger than necessary.

---

### 3.4.5 线性扫描：startScan / next / closeScan

**中文**

扫描 = 逐页逐槽遍历 + 条件过滤。`startScan` 把扫描状态（当前 page/slot、条件表达式、**扫描开始时的表总页数**）装进 `RM_ScanInfo`：

```c
RC startScan(RM_TableData *rel, RM_ScanHandle *scan, Expr *cond) {
    RM_ScanInfo *scanInfo = (RM_ScanInfo *) malloc(sizeof(RM_ScanInfo));
    if (scanInfo == NULL) return RC_RM_MEM_ALLOC_FAILED;

    scanInfo->curPage = 1;            // 数据从 page 1 开始
    scanInfo->curSlot = -1;
    scanInfo->condition = cond;

    // 关键：扫描一开始就把表的总页数固定下来，作为扫描上界
    scanInfo->totalPages = getTotalNumPages((BM_BufferPool *) rel->mgmtData);

    scan->rel = rel;
    scan->mgmtData = scanInfo;
    return RC_OK;
}
```

`next` 推进扫描，每次返回一条匹配的记录。核心是先看槽标记、跳过空槽/墓碑，再用 `evalExpr`（在 `expr.c` 里实现）对命中的记录求值条件：

```c
RC next(RM_ScanHandle *scan, Record *record) {
    RM_ScanInfo *scanInfo = (RM_ScanInfo *) scan->mgmtData;
    int recordSize = getRecordSize(scan->rel->schema);
    int numSlots = getRecordsPerPage(scan->rel->schema);

    while (true) {
        // 上界用扫描开始时的 totalPages。这里绝不能改成动态 getTotalNumPages()：
        // pinPage 碰到不存在的页会 appendEmptyBlock 扩文件，动态上界会「追着」
        // 不断变大的文件，永远扫不完。
        if (scanInfo->curPage >= scanInfo->totalPages)
            return RC_RM_NO_MORE_TUPLES;

        scanInfo->curSlot++;

        if (scanInfo->curSlot >= numSlots) {   // 当前页扫完，进下一页
            scanInfo->curPage++;
            scanInfo->curSlot = 0;
            continue;
        }

        BM_PageHandle pageHandle;
        if (pinPage(scan->rel->mgmtData, &pageHandle, scanInfo->curPage) != RC_OK)
            return RC_RM_BUFFER_PIN_FAILED;

        int offset = scanInfo->curSlot * (recordSize + 2);
        char marker = pageHandle.data[offset];
        if (marker != '+') {                   // 空槽/墓碑，跳过
            unpinPage(scan->rel->mgmtData, &pageHandle);
            continue;
        }

        memcpy(record->data, pageHandle.data + offset + 1, recordSize);
        record->id.page = scanInfo->curPage;
        record->id.slot = scanInfo->curSlot;

        if (scanInfo->condition == NULL) {     // 无条件扫描，直接返回
            unpinPage(scan->rel->mgmtData, &pageHandle);
            return RC_OK;
        }

        // 有条件：evalExpr 求值，命中才返回
        Value *result = NULL;
        if (evalExpr(record, scan->rel->schema, scanInfo->condition, &result) != RC_OK) {
            unpinPage(scan->rel->mgmtData, &pageHandle);
            return RC_RM_SCAN_CONDITION_EVAL_FAILED;
        }
        bool match = result->v.boolV;
        freeVal(result);
        unpinPage(scan->rel->mgmtData, &pageHandle);
        if (match) return RC_OK;
    }
}
```

`closeScan` 释放 `RM_ScanInfo`：

```c
RC closeScan(RM_ScanHandle *scan) {
    free(scan->mgmtData);
    scan->mgmtData = NULL;
    return RC_OK;
}
```

几个要点：

- **扫描上界是这道工序最容易踩的坑**：必须用 `startScan` 时固定的 `totalPages`。若改成每次 `next` 调动态 `getTotalNumPages()`，因为 `pinPage` 读不存在的页会扩文件，上界会跟着文件一起长大，扫描永远结束不了。
- **每槽一次 pin/unpin**：扫描逐槽 pin 页面、看完就 unpin，逻辑简单但每次都有缓冲池开销；课程规模可接受，第 4 章用 B+ 树把「逐表扫」变成「走索引」。
- **墓碑直接被跳过**：`'-'` 槽在标记检查这一层就被过滤，不会进入 `evalExpr`。
- 整个扫描的代价是 **O(表里所有记录)**——没有索引，纯线性。

**English**

A scan = page-by-page, slot-by-slot traversal with condition filtering. `startScan` packages the scan state (current page/slot, the condition expression, and the **table's total page count at scan start**) into an `RM_ScanInfo`:

```c
// (same code as above)
```

`next` advances the scan and returns one matching record per call. It first checks the slot marker (skipping empty and tombstone slots), then uses `evalExpr` (implemented in `expr.c`) to evaluate the condition on a live record:

```c
// (same code as above)
```

`closeScan` frees the `RM_ScanInfo`:

```c
// (same code as above)
```

Key points:

- **The scan bound is the easiest pitfall in this step**: it must be the `totalPages` captured at `startScan`. If you instead call the live `getTotalNumPages()` each iteration, the bound grows together with the file (because `pinPage` calls `appendEmptyBlock` for pages that do not exist yet) and the scan never terminates.
- **One pin/unpin per slot**: the scan pins a page, reads a slot, unpins — simple but with buffer-pool overhead per slot; fine at course scale. Chapter 4 replaces "scan the whole table" with "walk the index".
- **Tombstones are skipped early**: `'-'` slots are filtered at the marker check, so they never reach `evalExpr`.
- The whole scan costs **O(all records in the table)** — no index, pure linear.

---

## 3.5 完整 API 一览

| 函数 | 作用 | 一句话 |
|------|------|--------|
| `initRecordManager(NULL)` | 初始化 | 初始化 storage_mgr + 全局 buffer pool |
| `shutdownRecordManager()` | 关闭 | 释放全局 buffer pool |
| `createTable(name, schema)` | 建表 | 建文件 + 序列化 Schema 到 page 0 |
| `openTable(rel, name)` | 打开 | 读 page 0 还原 Schema，填到 `rel` |
| `closeTable(rel)` | 关闭 | free schema + shutdown buffer pool |
| `deleteTable(name)` | 删除 | 直接 `destroyPageFile` |
| `getNumTuples(rel)` | 元信息 | 返回表里当前 tuple 数 |
| `insertRecord(rel, record)` | 插入 | 逐页找空 → 写 → 回填 RID |
| `deleteRecord(rel, id)` | 删除 | 槽标记置 `'-'`（墓碑） |
| `updateRecord(rel, record)` | 更新 | 用 RID 定位后整条覆盖 |
| `getRecord(rel, id, record)` | 点查 | 按 `(page, slot)` 读一条 |
| `startScan(rel, scan, cond)` | 开始扫描 | 初始化 `RM_ScanInfo`（含固定的 `totalPages`） |
| `next(scan, record)` | 取下一条 | 逐页逐槽 + 标记过滤 + `evalExpr` |
| `closeScan(scan)` | 结束扫描 | 释放 `RM_ScanInfo` |
| `getRecordSize(schema)` | 算大小 | 各列长度累加 |
| `createSchema(...)` / `freeSchema` | Schema 生命周期 | — |
| `createRecord(...)` / `freeRecord` | Record 生命周期 | — |
| `getAttr` / `setAttr` | 属性读写 | 按 offset 取/写一列 |

本实现把元组数 `numTuples` **直接持久化**到 page 0：`closeTable` 在关表前写回，
`openTable` 读回并赋给 `rel->numTuples`，因此关闭后重新打开计数仍然正确。

墓碑是槽头独立的标记字节（`'-'`），与记录数据分离，不会误判正常记录。生产级
页布局通常会进一步用 slot 状态位图或 tuple header 标志来支持更多槽状态。

---

## 3.6 编译与验证

```bash
# 编译整个项目
make all

# 运行第 3 章对应的测试（建表 + 增删改查 + 扫描）
./build/test_assign3_1
```

最小验证：建一张表 → 插几条 → 扫描看是否返回正确结果。

```c
Schema *schema = createSchema(3,
    (char*[]){"a", "b", "c"},                          // 3 列
    (DataType[]){DT_INT, DT_STRING, DT_FLOAT},
    (int[]){0, 4, 0},                                  // b 是 4 字节定长串
    1, (int[]){0});                                    // 主键是第 0 列

initRecordManager(NULL);
createTable("test_tbl", schema);
RM_TableData rel;
openTable(&rel, "test_tbl");

Record *r;
createRecord(&r, schema);
// … setAttr 填值 …
insertRecord(&rel, r);
printf("tuples: %d\n", getNumTuples(&rel));           // 1
```

---

## 3.7 思考题

1. **本实现的墓碑是槽头独立的标记字节 `'-'`，与记录数据分离，因此不会误判正常记录。但如果要支持更多槽状态（未用 / 占用 / 已删 / 已更新），1 个标记字节还够吗？槽状态应该怎么编码？** 提示：引入槽状态位图（free space bitmap）或给标记字节增加取值。

2. **`insertRecord` 现在插入时扫描页内第一个非 `'+'` 槽（复用墓碑/空槽）。如果表需要频繁的「删中插」，「每次插入都从槽 0 重新扫描」仍然是个代价。有什么办法避免重复扫描？** 提示：页内空闲槽链表，或页头记录一个「下一个可能空闲」的 free hint。

3. **线性扫描的上界是 `startScan` 时固定的 `totalPages`。如果把它改成 `next` 里每次调用 `getTotalNumPages()`，会发生什么？为什么？** 提示：`pinPage` 对不存在的页做了什么（第 2 章）。

---

> **下一章**：[第4章 · B+树索引](04-btree-index.md)
