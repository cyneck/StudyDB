# 第3章 · 记录管理器 Record Manager

> 对应源文件：`record_mgr.c` / `record_mgr.h` / `record_mgr_ex.h` / `tables.h`
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
│ Schema   │  数据页 1    │  数据页 2    │     │  数据页 N    │
│ (序列化) │              │              │     │              │
└──────────┴──────────────┴──────────────┴─────┴──────────────┘
```

- **page 0** 存「Schema 元信息」：序列化后的 `Schema` + 表的当前数据页数 `numPageOfTable`
- **page 1..N** 是**数据页**，每页内部布局：

```
┌──────────────┬──────────┬──────────┬─────┬──────────┐
│ numRecords  │ record 0 │ record 1 │ ... │ record K │
│ (4 bytes)   │          │          │     │          │
└──────────────┴──────────┴──────────┴─────┴──────────┘
                  ↑ slot 0   ↑ slot 1        ↑ slot K
```

`numRecords` 既记录当前页有多少条记录，又兼任「页满」标记——页满了就置 `-1`，下次插入直接开新页。

**记录本身**是一条紧凑的字节串，各属性按 Schema 顺序拼接，按 offset 访问：

```
┌──────────┬──────────┬──────────┬─────┐
│  attr 0  │  attr 1  │  attr 2  │ ... │
│ offset 0 │ offset S0│ offset S0+S1 │   │
└──────────┴──────────┴──────────┴─────┘
```

**RID → 字节偏移公式**：给定 `RID = (page, slot)`，记录在所在页内的字节偏移是

```
offset = slot × recordSize + sizeof(int)
                                ↑ 跳过 numRecords 头
```

**English**

A table file on disk looks like this:

```
┌──────────┬──────────────┬──────────────┬─────┬──────────────┐
│  page 0  │   page 1     │   page 2     │ ... │   page N     │
│ Schema   │  data pg 1   │  data pg 2   │     │  data pg N   │
│ (serial) │              │              │     │              │
└──────────┴──────────────┴──────────────┴─────┴──────────────┘
```

- **page 0** stores Schema metadata: the serialized `Schema` + the current number of data pages `numPageOfTable`.
- **page 1..N** are **data pages**. Each data page is laid out as:

```
┌──────────────┬──────────┬──────────┬─────┬──────────┐
│ numRecords  │ record 0 │ record 1 │ ... │ record K │
│ (4 bytes)   │          │          │     │          │
└──────────────┴──────────┴──────────┴─────┴──────────┘
                  ↑ slot 0   ↑ slot 1        ↑ slot K
```

`numRecords` both counts records in the page and acts as a "page full" marker — set to `-1` when the page is full, so the next insert goes straight to a new page.

**A record** is a compact byte string: attributes are concatenated in Schema order and accessed by offset.

**RID → byte offset formula**: given `RID = (page, slot)`, the record's in-page byte offset is

```
offset = slot × recordSize + sizeof(int)
                                ↑ skip numRecords header
```

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

扩展类型（`record_mgr_ex.h`，**内部使用，不对用户暴露**）：

```c
typedef struct RM_PageSlot  { int page_id; int slot_id; } PageSlot;        // 内部定位
typedef struct RM_TableInfo { int numOfTuples; } TableInfo;                // 表级元数据
typedef struct RM_Scanner   { int page; int slot; Expr *cond; } Scanner;  // 扫描状态
```

`RM_TableData` 是用户拿到表的句柄：

```c
typedef struct RM_TableData {
    char *name;
    Schema *schema;
    void *mgmtData;   // 装着 TableInfo（当前表里有多少条 tuple）
} RM_TableData;
```

`mgmtData` 用 `void*` 跟第1章的 `SM_FileHandle.mgmtInfo` 是一个套路——信息隐藏，让上层只关心表语义，不暴露内部 `TableInfo` 结构。

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

Extension types (`record_mgr_ex.h`, **internal, not exposed to users**):

```c
typedef struct RM_PageSlot  { int page_id; int slot_id; } PageSlot;        // internal location
typedef struct RM_TableInfo { int numOfTuples; } TableInfo;                // table-level metadata
typedef struct RM_Scanner   { int page; int slot; Expr *cond; } Scanner;  // scan state
```

`RM_TableData` is the user-facing table handle:

```c
typedef struct RM_TableData {
    char *name;
    Schema *schema;
    void *mgmtData;   // holds a TableInfo (how many tuples are in the table)
} RM_TableData;
```

`mgmtData` is `void*` for the same information-hiding reason as `SM_FileHandle.mgmtInfo` in Chapter 1 — upper layers see only table semantics, not the internal `TableInfo` struct.

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

注意：字符串是**定长**的（长度由 `typeLength[i]` 决定），不是 C 字符串那种以 `\0` 结尾的变长。这样每条记录长度都相同，第 N 条记录的偏移就能用 `slot × recordSize` 直接算出来——这是「定长记录」模型的关键权衡。

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

Note: strings are **fixed-length** (length given by `typeLength[i]`), not C-string-style variable-with-`\0`. This keeps every record the same length, so the Nth record's offset is simply `slot × recordSize`. This fixed-length-record trade-off is what makes slot-based addressing work.

---

### 3.4.2 Schema 持久化：saveTableSchema / readTableSchema

**中文**

表关掉再打开时，`Schema` 必须能从磁盘读回。`saveTableSchema` 把 Schema 序列化到 page 0（schema 太大可能溢出到 page 1+，所以 `numPagesOfSchema` 也一并记下）：

```c
RC saveTableSchema(Schema *schema) {
    int sizeSchema = sizeof(int) * (3 + schema->numAttr * 3 + schema->keySize)
                   + sizeof(DataType) * schema->numAttr;
    int attrNameOffset = sizeSchema;            // 字符串区从元数据区之后开始
    int offset = 0;
    int i;
    for (i = 0; i < schema->numAttr; i++)
        sizeSchema += strlen(schema->attrNames[i]) + 1;

    int numPagesOfSchema = (sizeSchema - 1) / PAGE_SIZE + 1;
    char *buffer = (char *) malloc(PAGE_SIZE * numPagesOfSchema);
    memset(buffer, '\0', PAGE_SIZE * numPagesOfSchema);
    int numPageOfTable = 0;

    MEMCPY_TO_OFFSET(&numPagesOfSchema, int);
    MEMCPY_TO_OFFSET(&numPageOfTable, int);
    MEMCPY_TO_OFFSET(&(schema->numAttr), int);
    MEMCPY_TO_OFFSET(&(schema->keySize), int);

    for (i = 0; i < schema->numAttr; i++) {
        int slen = strlen(schema->attrNames[i]) + 1;
        MEMCPY_TO_OFFSET(&attrNameOffset, int);
        MEMCPY_TO_OFFSET(&slen, int);
        MEMCPY_TO_OFFSET(&(schema->dataTypes[i]), DataType);
        MEMCPY_TO_OFFSET(&(schema->typeLength[i]), int);
        memcpy(&(buffer[attrNameOffset]), schema->attrNames[i], slen);
        attrNameOffset += slen;
    }
    for (i = 0; i < schema->keySize; i++)
        MEMCPY_TO_OFFSET(&(schema->keyAttrs[i]), int);

    for (i = 0; i < numPagesOfSchema; i++) {
        pinPage(pBuffP, pPageH, i);
        memcpy(pPageH->data, &(buffer[i * PAGE_SIZE]), PAGE_SIZE);
        markDirty(pBuffP, pPageH);
        unpinPage(pBuffP, pPageH);
    }
    free(buffer);
    return RC_OK;
}
```

page 0 的字节布局：

```
┌──────────────┬──────────────┬─────────┬─────────┬───────────────┬─────────────┐
│numPagesOfSchem│numPageOfTable│ numAttr │ keySize │ 每列元信息×N  │ keyAttrs×K  │ ... │ attrNames 区 │
│   (int)       │   (int)      │ (int)   │ (int)   │ offset,slen,  │  (int×K)    │     │ (字符串拼接) │
│              │              │         │         │ dataType,typLen│            │     │              │
└──────────────┴──────────────┴─────────┴─────────┴───────────────┴─────────────┘
```

`MEMCPY_TO_OFFSET` 是 `record_mgr_ex.h` 里的宏：把数据拷到 `buffer[offset]` 然后把 `offset` 往前推 `sizeof(type)` 字节——等价于「写入 + seek」：

```c
#define MEMCPY_TO_OFFSET(__expression__, __type__)               \
    memcpy(&(buffer[offset]), __expression__, sizeof(__type__)); \
    offset += sizeof(__type__)
```

`readTableSchema` 是镜像操作：先从 page 0 读出 `numPagesOfSchema`，把那几页读进 buffer，然后按相反顺序反序列化回 `Schema*`。

**English**

When a table is closed and reopened, its Schema must be readable from disk. `saveTableSchema` serializes the Schema into page 0 (large schemas may spill onto page 1+, so `numPagesOfSchema` is also stored):

```c
// (same code as above)
```

Page 0 byte layout:

```
┌──────────────┬──────────────┬─────────┬─────────┬───────────────┬─────────────┐
│numPagesOfSchem│numPageOfTable│ numAttr │ keySize │ per-attr meta×N│ keyAttrs×K  │ ... │ attrNames area │
│   (int)       │   (int)      │ (int)   │ (int)   │ offset,slen,  │  (int×K)    │     │ (concatenated) │
│              │              │         │         │ dataType,typLen│            │     │              │
└──────────────┴──────────────┴─────────┴─────────┴───────────────┴─────────────┘
```

`MEMCPY_TO_OFFSET` is a macro in `record_mgr_ex.h`: it copies data into `buffer[offset]` and then advances `offset` by `sizeof(type)` — equivalent to "write + seek":

```c
#define MEMCPY_TO_OFFSET(__expression__, __type__)               \
    memcpy(&(buffer[offset]), __expression__, sizeof(__type__)); \
    offset += sizeof(__type__)
```

`readTableSchema` is the mirror operation: it first reads `numPagesOfSchema` from page 0, loads those pages into a buffer, then deserializes back into a `Schema*` in reverse order.

---

### 3.4.3 插入记录：insertRecord

**中文**

`insertRecord` 的流程：**读 `numPageOfTable` → 找末页 → 看是否满 → 写入 → 更新 `numRecords` → 回填 RID**。

```c
RC insertRecord(RM_TableData *rel, Record *record) {
    int numPagesOfTable = 0;
    pinPage(pBuffP, pPageH, 0);
    memcpy(&numPagesOfTable, pPageH->data + sizeof(int), sizeof(int)); // 从 page 0 读数据页数
    unpinPage(pBuffP, pPageH);

    Schema *schema = rel->schema;
    PageSlot pos;
    int numRecordInPage = 0;

    if (0 == numPagesOfTable) {                                 // 表里还没有数据页
        numPagesOfTable++;
        updateNumPageOfTable(numPagesOfTable);
        numRecordInPage = 0;
        pinPage(pBuffP, pPageH, 1);                            // 初始化第 1 页
        memcpy(pPageH->data, &numRecordInPage, sizeof(int));
        markDirty(pBuffP, pPageH); unpinPage(pBuffP, pPageH);
        forceFlushPool(pBuffP);
        pos.page_id = numPagesOfTable; pos.slot_id = 0;
    } else {                                                    // 读末页的 numRecords
        int tmpNum = 0;
        pinPage(pBuffP, pPageH, numPagesOfTable);
        memcpy(&tmpNum, pPageH->data, sizeof(int));
        unpinPage(pBuffP, pPageH);
        pos.page_id = numPagesOfTable; pos.slot_id = tmpNum;

        if (tmpNum == -1) {                                     // 末页已满，开新页
            numPagesOfTable++;
            updateNumPageOfTable(numPagesOfTable);
            numRecordInPage = 0;
            pinPage(pBuffP, pPageH, numPagesOfTable);
            memcpy(pPageH->data, &numRecordInPage, sizeof(int));
            markDirty(pBuffP, pPageH); unpinPage(pBuffP, pPageH);
            forceFlushPool(pBuffP);
            pos.page_id = numPagesOfTable; pos.slot_id = 0;
        }
    }

    record->id.page = pos.page_id;
    record->id.slot = pos.slot_id;

    int recordSize = getRecordSize(schema);
    int offset = pos.slot_id * recordSize + sizeof(int);        // 跳过 numRecords 头
    pinPage(pBuffP, pPageH, numPagesOfTable);
    memcpy((char *) pPageH->data + offset, record->data, recordSize);

    numRecordInPage = pos.slot_id + 1;
    if ((numRecordInPage + 1) * recordSize + sizeof(int) > PAGE_SIZE)
        numRecordInPage = -1;                                   // 写完这条页就满，标记 -1
    memcpy(pPageH->data, &numRecordInPage, sizeof(int));
    markDirty(pBuffP, pPageH); unpinPage(pBuffP, pPageH);
    forceFlushPool(pBuffP);

    numOfTuples++;
    ((TableInfo *) rel->mgmtData)->numOfTuples++;
    return RC_OK;
}
```

关键点：

- **末页满标记**：`numRecords = -1` 表示「这页满了，下一条插入直接开新页」，避免了反复扫描找空槽。
- **RID 即返回值**：插入成功后 `record->id` 被填上 `(page, slot)`，调用方拿到就能用于后续 `getRecord` / `deleteRecord`。
- **页满判断靠算术**：因为 `recordSize` 固定，`(numRecordInPage + 1) * recordSize + sizeof(int) > PAGE_SIZE` 这一简单的算术就能判断「下一条还塞不塞得下」。
- **`markDirty` + `forceFlushPool`**：写完立即刷盘，保证崩溃一致性；代价是性能（每次插入都触发 I/O）。

**English**

`insertRecord` flow: **read `numPageOfTable` → find last page → check if full → write → update `numRecords` → backfill RID**.

```c
// (same code as above)
```

Key points:

- **Full-page marker**: `numRecords = -1` means "this page is full, next insert goes to a new page" — avoids repeatedly scanning for free slots.
- **RID is the return value**: on success, `record->id` is filled with `(page, slot)`, which the caller can pass to `getRecord` / `deleteRecord`.
- **Page-full check by arithmetic**: because `recordSize` is constant, the simple check `(numRecordInPage + 1) * recordSize + sizeof(int) > PAGE_SIZE` is enough to tell whether the next record still fits.
- **`markDirty` + `forceFlushPool`**: flush to disk immediately after each write for crash consistency — at the cost of performance (every insert triggers I/O).

---

### 3.4.4 删除与墓碑：deleteRecord

**中文**

直接把记录从页里抹掉会破坏 `slot × recordSize` 的偏移公式（后面的记录会错位）。本实现用了最朴素的**墓碑（tombstone）**：把记录头三个字节写成 `-D-`，物理上不删，逻辑上视为已删。

```c
RC deleteRecord(RM_TableData *rel, RID id) {
    int page_id = id.page, slot_id = id.slot;
    int recordSize = getRecordSize(rel->schema);

    char *data = (char *) malloc(sizeof(char) * recordSize);
    memset(data, '\0', sizeof(char) * recordSize);
    data[0] = '-'; data[1] = 'D'; data[2] = '-';

    int offset = slot_id * recordSize + sizeof(int);
    pinPage(pBuffP, pPageH, page_id);
    memcpy((char *) pPageH->data + offset, data, recordSize);
    markDirty(pBuffP, pPageH); unpinPage(pBuffP, pPageH);
    forceFlushPool(pBuffP);
    free(data);

    numOfTuples--;
    ((TableInfo *) rel->mgmtData)->numOfTuples--;
    return RC_OK;
}
```

`getRecord` 读出后会检查头三字节是不是 `-D-`，是就返回 `RC_RM_NO_MORE_TUPLES`（墓碑标记）：

```c
if ('-' == record->data[0] && 'D' == record->data[1] && '-' == record->data[2])
    return RC_RM_NO_MORE_TUPLES;
```

扫描时遇到墓碑就跳过。这是最简单的「软删除」——

- **优点**：实现极简，不影响 RID 寻址，`RID = (page, slot)` 在删除前后含义一致
- **缺点**：空间不回收，反复增删会让文件越来越「虚」；后续若要复用空槽，还得额外维护一个空闲链表

**English**

Directly erasing a record from a page would break the `slot × recordSize` offset formula (subsequent records would shift). This implementation uses the simplest form of **tombstone**: write `-D-` into the first three bytes of the record's data. Physically nothing is removed; logically the record is gone.

```c
// (same code as above)
```

After `getRecord` reads a record, it checks the first three bytes for `-D-`; if so, it returns `RC_RM_NO_MORE_TUPLES` (the tombstone marker):

```c
if ('-' == record->data[0] && 'D' == record->data[1] && '-' == record->data[2])
    return RC_RM_NO_MORE_TUPLES;
```

Scans skip tombstoned records. This is the simplest "soft delete":

- **Pro**: dead-simple, doesn't disturb RID addressing — `RID = (page, slot)` means the same thing before and after a delete.
- **Con**: space is never reclaimed; repeated inserts/deletes leave the file increasingly "hollow". Reusing free slots later would require an additional free-list.

---

### 3.4.5 线性扫描：startScan / next / closeScan

**中文**

扫描 = 逐页逐槽遍历 + 条件过滤。`startScan` 把扫描状态（当前 page/slot + 条件表达式）装进 `Scanner`：

```c
RC startScan(RM_TableData *rel, RM_ScanHandle *scan, Expr *cond) {
    if (!rel || !scan || !cond) return RC_NULL_POINTER;

    int numPages = 0;
    pinPage(pBuffP, pPageH, 0);
    memcpy(&numPages, pPageH->data + sizeof(int), sizeof(int)); // 数据页数
    unpinPage(pBuffP, pPageH);

    Scanner *sc = (Scanner *) malloc(sizeof(Scanner));
    sc->page = numPages;
    sc->slot = 0;
    sc->cond = cond;
    scan->rel = rel;
    scan->mgmtData = sc;
    return RC_OK;
}
```

`next` 推进扫描，每次返回一条匹配的记录。核心是用 `evalExpr`（在 `expr.c` 里实现）对每条记录求值条件表达式：

```c
RC next(RM_ScanHandle *scan, Record *record) {
    RM_TableData *rel = scan->rel;
    Scanner *sc = (Scanner *) scan->mgmtData;
    int page = sc->page, slot = sc->slot;
    int recordSize = getRecordSize(rel->schema);
    int pageNum = sc->page + 1;
    RID rid;

    Record *tmpRecord = (Record *) malloc(sizeof(Record));
    tmpRecord->data = (char *) malloc(sizeof(char) * recordSize);
    Value *value;
    while (true) {
        int offset = page * PAGE_SIZE + sizeof(int) + slot * recordSize;
        if (offset > pageNum * PAGE_SIZE) {              // 越界 = 扫完
            freeRecord(tmpRecord);
            return RC_RM_NO_MORE_TUPLES;
        }
        rid.page = page; rid.slot = slot;
        getRecord(rel, rid, tmpRecord);                  // 取一条
        evalExpr(tmpRecord, rel->schema, sc->cond, &value); // 求值条件

        if (value->v.boolV) {                            // 命中
            memcpy(record->data, tmpRecord->data, recordSize);
            // 推进扫描游标 page/slot …
            freeVal(value); break;
        }
        freeVal(value);
        // 没命中，游标前进 …
    }
    freeRecord(tmpRecord);
    return RC_OK;
}
```

`closeScan` 释放 `Scanner`：

```c
RC closeScan(RM_ScanHandle *scan) {
    if (scan->mgmtData) free(scan->mgmtData);
    return RC_OK;
}
```

整个扫描的代价是 **O(表里所有记录)**——没有索引，纯线性。第 4 章会用 B+ 树把这点优化掉。

**English**

A scan = page-by-page, slot-by-slot traversal with condition filtering. `startScan` packages the scan state (current page/slot + condition expression) into a `Scanner`:

```c
// (same code as above)
```

`next` advances the scan and returns one matching record per call. The core is `evalExpr` (implemented in `expr.c`) which evaluates the condition against each record:

```c
// (same code as above)
```

`closeScan` frees the `Scanner`:

```c
// (same code as above)
```

The whole scan costs **O(all records in the table)** — no index, pure linear. Chapter 4 will optimize this away with a B+ tree.

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
| `insertRecord(rel, record)` | 插入 | 找末页/新页 → 写 → 回填 RID |
| `deleteRecord(rel, id)` | 删除 | 写 `-D-` 墓碑 |
| `updateRecord(rel, record)` | 更新 | 用 RID 定位后整条覆盖 |
| `getRecord(rel, id, record)` | 点查 | 按 `(page, slot)` 读一条 |
| `startScan(rel, scan, cond)` | 开始扫描 | 初始化 `Scanner` |
| `next(scan, record)` | 取下一条 | 逐页逐槽 + `evalExpr` 过滤 |
| `closeScan(scan)` | 结束扫描 | 释放 `Scanner` |
| `getRecordSize(schema)` | 算大小 | 各列长度累加 |
| `createSchema(...)` / `freeSchema` | Schema 生命周期 | — |
| `createRecord(...)` / `freeRecord` | Record 生命周期 | — |
| `getAttr` / `setAttr` | 属性读写 | 按 offset 取/写一列 |

`openTable` 会根据数据页头和墓碑重新计算存活 tuple 数，因此关闭后重新打开仍能
得到正确计数，而不必额外持久化一个可能与记录状态漂移的计数器。

`-D-` 墓碑只是教学简化：它占用了用户数据的前三个字节，因此正常记录如果恰好
以这三个字节开头，会被误判为已删除。生产级页布局应使用独立的 slot 状态位图或
tuple header 标志。

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

1. **本实现的墓碑 `-D-` 写在记录头三个字节。如果某个属性列本身就是 `DT_STRING`，且某条正常记录前三个字节恰好等于 `-D-`，会发生什么误判？应该把墓碑放在哪里才能彻底避免这种冲突？** 提示：考虑给记录加一个独立的「有效/已删」标志位，而不是和数据区共用字节。

2. **页满标记用 `numRecords = -1`，下次插入走新页。但被删记录留下的空槽永远不复用——文件会越长越「虚」。如果要支持空槽复用，文件头（page 0）或每个数据页头部还需要多存什么信息？** 提示：空闲槽链表 / free space bitmap。

3. **线性扫描里 `getRecord` 在遇到墓碑时返回 `RC_RM_NO_MORE_TUPLES`，但扫描器其实希望「跳过墓碑继续找」而不是「停止扫描」。这两者怎么协调？** 提示：仔细看 `next` 里的 `while(true)` 循环——它如何把「这是墓碑，下一条」和「真的扫完了」区分开。

---

> **下一章**：[第4章 · B+树索引](04-btree-index.md)
