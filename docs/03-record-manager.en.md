# Chapter 3 · Record Manager

> Source files: `record_mgr.c` / `record_mgr.h` / `record_mgr_ex.h` / `tables.h`
>
> Chapter 1 abstracted disk into "a sequence of pages"; Chapter 2 caches pages in memory. But the user's view has no "pages" — they see "tables, rows, columns". The **Record Manager** is the translator above those two layers: it organizes fixed-length pages into "a set of records with a schema".

---

## 3.1 Why this layer is needed

The storage manager only knows "which page"; the buffer pool only knows "pin / unpin a page". But the SQL-level user view is quite different:

- A table has a name, a set of columns, each with a type
- Rows can be inserted, deleted, updated, queried
- Queries filter rows by condition and return **rows**, not bytes

There's a big gap here: **fixed 4096-byte pages vs. a collection of variable-length records**. The record manager fills that gap. It does three things:

1. **Describe structure** — use `Schema` to describe "how many columns, what they're called, what types"
2. **Locate records** — use `RID = (page, slot)` to uniquely identify a row
3. **CRUD + scan** — provide table-semantics APIs like `insertRecord` / `deleteRecord` / `updateRecord` / `getRecord` / `startScan` on top of pages

---

## 3.2 Core principle: page organization of a table file

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

## 3.3 Key data structures

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

## 3.4 Key code walkthrough

### 3.4.1 How big is a record: getRecordSize

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

### 3.4.2 Schema persistence: saveTableSchema / readTableSchema

When a table is closed and reopened, its Schema must be readable from disk. `saveTableSchema` serializes the Schema into page 0 (large schemas may spill onto page 1+, so `numPagesOfSchema` is also stored):

```c
RC saveTableSchema(Schema *schema) {
    int sizeSchema = sizeof(int) * (3 + schema->numAttr * 3 + schema->keySize)
                   + sizeof(DataType) * schema->numAttr;
    int attrNameOffset = sizeSchema;            // string area starts after metadata area
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

### 3.4.3 Inserting a record: insertRecord

`insertRecord` flow: **read `numPageOfTable` → find last page → check if full → write → update `numRecords` → backfill RID**.

```c
RC insertRecord(RM_TableData *rel, Record *record) {
    int numPagesOfTable = 0;
    pinPage(pBuffP, pPageH, 0);
    memcpy(&numPagesOfTable, pPageH->data + sizeof(int), sizeof(int)); // read data-page count
    unpinPage(pBuffP, pPageH);

    Schema *schema = rel->schema;
    PageSlot pos;
    int numRecordInPage = 0;

    if (0 == numPagesOfTable) {                                 // table has no data pages yet
        numPagesOfTable++;
        updateNumPageOfTable(numPagesOfTable);
        numRecordInPage = 0;
        pinPage(pBuffP, pPageH, 1);                            // initialize page 1
        memcpy(pPageH->data, &numRecordInPage, sizeof(int));
        markDirty(pBuffP, pPageH); unpinPage(pBuffP, pPageH);
        forceFlushPool(pBuffP);
        pos.page_id = numPagesOfTable; pos.slot_id = 0;
    } else {                                                    // read last page's numRecords
        int tmpNum = 0;
        pinPage(pBuffP, pPageH, numPagesOfTable);
        memcpy(&tmpNum, pPageH->data, sizeof(int));
        unpinPage(pBuffP, pPageH);
        pos.page_id = numPagesOfTable; pos.slot_id = tmpNum;

        if (tmpNum == -1) {                                     // last page is full, open a new one
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
    int offset = pos.slot_id * recordSize + sizeof(int);        // skip numRecords header
    pinPage(pBuffP, pPageH, numPagesOfTable);
    memcpy((char *) pPageH->data + offset, record->data, recordSize);

    numRecordInPage = pos.slot_id + 1;
    if ((numRecordInPage + 1) * recordSize + sizeof(int) > PAGE_SIZE)
        numRecordInPage = -1;                                   // mark -1 if writing this record fills the page
    memcpy(pPageH->data, &numRecordInPage, sizeof(int));
    markDirty(pBuffP, pPageH); unpinPage(pBuffP, pPageH);
    forceFlushPool(pBuffP);

    numOfTuples++;
    ((TableInfo *) rel->mgmtData)->numOfTuples++;
    return RC_OK;
}
```

Key points:

- **Full-page marker**: `numRecords = -1` means "this page is full, next insert goes to a new page" — avoids repeatedly scanning for free slots.
- **RID is the return value**: on success, `record->id` is filled with `(page, slot)`, which the caller can pass to `getRecord` / `deleteRecord`.
- **Page-full check by arithmetic**: because `recordSize` is constant, the simple check `(numRecordInPage + 1) * recordSize + sizeof(int) > PAGE_SIZE` is enough to tell whether the next record still fits.
- **`markDirty` + `forceFlushPool`**: flush to disk immediately after each write for crash consistency — at the cost of performance (every insert triggers I/O).

---

### 3.4.4 Deletion and tombstones: deleteRecord

Directly erasing a record from a page would break the `slot × recordSize` offset formula (subsequent records would shift). This implementation uses the simplest form of **tombstone**: write `-D-` into the first three bytes of the record's data. Physically nothing is removed; logically the record is gone.

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

After `getRecord` reads a record, it checks the first three bytes for `-D-`; if so, it returns `RC_RM_NO_MORE_TUPLES` (the tombstone marker):

```c
if ('-' == record->data[0] && 'D' == record->data[1] && '-' == record->data[2])
    return RC_RM_NO_MORE_TUPLES;
```

Scans skip tombstoned records. This is the simplest "soft delete":

- **Pro**: dead-simple, doesn't disturb RID addressing — `RID = (page, slot)` means the same thing before and after a delete.
- **Con**: space is never reclaimed; repeated inserts/deletes leave the file increasingly "hollow". Reusing free slots later would require an additional free-list.

---

### 3.4.5 Linear scan: startScan / next / closeScan

A scan = page-by-page, slot-by-slot traversal with condition filtering. `startScan` packages the scan state (current page/slot + condition expression) into a `Scanner`:

```c
RC startScan(RM_TableData *rel, RM_ScanHandle *scan, Expr *cond) {
    if (!rel || !scan || !cond) return RC_NULL_POINTER;

    int numPages = 0;
    pinPage(pBuffP, pPageH, 0);
    memcpy(&numPages, pPageH->data + sizeof(int), sizeof(int)); // data page count
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

`next` advances the scan and returns one matching record per call. The core is `evalExpr` (implemented in `expr.c`) which evaluates the condition against each record:

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
        if (offset > pageNum * PAGE_SIZE) {              // out of bounds = scan done
            freeRecord(tmpRecord);
            return RC_RM_NO_MORE_TUPLES;
        }
        rid.page = page; rid.slot = slot;
        getRecord(rel, rid, tmpRecord);                  // fetch one record
        evalExpr(tmpRecord, rel->schema, sc->cond, &value); // evaluate condition

        if (value->v.boolV) {                            // hit
            memcpy(record->data, tmpRecord->data, recordSize);
            // advance scan cursor page/slot …
            freeVal(value); break;
        }
        freeVal(value);
        // no hit, advance cursor …
    }
    freeRecord(tmpRecord);
    return RC_OK;
}
```

`closeScan` frees the `Scanner`:

```c
RC closeScan(RM_ScanHandle *scan) {
    if (scan->mgmtData) free(scan->mgmtData);
    return RC_OK;
}
```

The whole scan costs **O(all records in the table)** — no index, pure linear. Chapter 4 will optimize this away with a B+ tree.

---

## 3.5 Complete API overview

| Function | Purpose | One-liner |
|----------|---------|-----------|
| `initRecordManager(NULL)` | Initialize | Initialize storage_mgr + global buffer pool |
| `shutdownRecordManager()` | Shutdown | Free global buffer pool |
| `createTable(name, schema)` | Create table | Create file + serialize Schema to page 0 |
| `openTable(rel, name)` | Open | Read page 0 to restore Schema, fill `rel` |
| `closeTable(rel)` | Close | free schema + shutdown buffer pool |
| `deleteTable(name)` | Delete | Directly `destroyPageFile` |
| `getNumTuples(rel)` | Metadata | Return current tuple count |
| `insertRecord(rel, record)` | Insert | Find last/new page → write → backfill RID |
| `deleteRecord(rel, id)` | Delete | Write `-D-` tombstone |
| `updateRecord(rel, record)` | Update | Locate by RID, overwrite the whole record |
| `getRecord(rel, id, record)` | Point query | Read one record by `(page, slot)` |
| `startScan(rel, scan, cond)` | Start scan | Initialize `Scanner` |
| `next(scan, record)` | Next | Page-by-page + `evalExpr` filtering |
| `closeScan(scan)` | End scan | Free `Scanner` |
| `getRecordSize(schema)` | Size | Sum of column lengths |
| `createSchema(...)` / `freeSchema` | Schema lifecycle | — |
| `createRecord(...)` / `freeRecord` | Record lifecycle | — |
| `getAttr` / `setAttr` | Attr read/write | Get/set one column by offset |

`openTable` reconstructs the live tuple count from data-page headers and
tombstones. The count therefore survives close/reopen without a second
persisted counter that could drift from record state.

The `-D-` tombstone is a teaching simplification: because it shares the first
three bytes of user data, a valid record beginning with those bytes is
indistinguishable from a deleted row. A production layout needs a separate
slot-state bitmap or tuple-header flag.

---

## 3.6 Compilation and verification

```bash
# Build the whole project
make all

# Run the tests for Chapter 3 (create table + CRUD + scan)
./build/test_assign3_1
```

Minimal verification: create a table → insert a few records → scan and check the result.

```c
Schema *schema = createSchema(3,
    (char*[]){"a", "b", "c"},                          // 3 columns
    (DataType[]){DT_INT, DT_STRING, DT_FLOAT},
    (int[]){0, 4, 0},                                  // b is a 4-byte fixed-length string
    1, (int[]){0});                                    // primary key is column 0

initRecordManager(NULL);
createTable("test_tbl", schema);
RM_TableData rel;
openTable(&rel, "test_tbl");

Record *r;
createRecord(&r, schema);
// … setAttr to fill values …
insertRecord(&rel, r);
printf("tuples: %d\n", getNumTuples(&rel));           // 1
```

---

## 3.7 Discussion questions

1. **This implementation writes the tombstone `-D-` into the first three bytes of a record. If some attribute column is itself `DT_STRING`, and a normal record happens to have its first three bytes equal to `-D-`, what false positive would occur? Where should the tombstone be placed to fully avoid this conflict?** Hint: consider adding a separate "valid/deleted" flag bit to the record, rather than sharing bytes with the data area.

2. **The page-full marker uses `numRecords = -1`, and the next insert goes to a new page. But the free slots left by deleted records are never reused — the file grows increasingly "hollow". If you wanted to support free-slot reuse, what extra information would the file header (page 0) or each data page header need to store?** Hint: free-slot list / free space bitmap.

3. **In the linear scan, `getRecord` returns `RC_RM_NO_MORE_TUPLES` when it hits a tombstone — but the scanner actually wants to "skip the tombstone and keep looking", not "stop the scan". How do these two coordinate?** Hint: look carefully at the `while(true)` loop in `next` — how does it distinguish "this is a tombstone, on to the next" from "the scan is genuinely done"?

---

> **Next chapter**: [Chapter 4 · B+ Tree Index](04-btree-index.en.md)
