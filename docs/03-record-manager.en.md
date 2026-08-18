# Chapter 3 · Record Manager

> Source files: `record_mgr.c` / `record_mgr.h` / `tables.h`
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

Note: strings are **fixed-length** (length given by `typeLength[i]`), not C-string-style variable-with-`\0`. This keeps every record the same length, so the Nth slot's offset is simply `slot × (recordSize + 2)` (the +2 being the slot's marker byte). This fixed-length-record + marker-slot trade-off is what makes slot-based addressing work.

---

### 3.4.2 Schema persistence: writeTableSchema / openTable

When a table is closed and reopened, its Schema must be readable from disk. Our `writeTableSchema` serializes the Schema + tuple count into page 0 (large schemas may spill onto page 1+, so `numPagesOfSchema` is stored too). Note it writes **directly through the storage manager** (`writeBlock`) rather than through the buffer pool — at create time the table is not open, so no pool exists yet:

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

    int attrNameOffset = sizeSchema;   // string area starts after the metadata area
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

### 3.4.3 Inserting a record: insertRecord

`insertRecord` flow: **walk pages from page 1 → find the first free slot → if there is one, insert → backfill RID**. No data-page count is kept on page 0 — `pinPage` auto-grows the file when asked for a page that does not exist yet (chapter 2), so the loop naturally opens a new page when it reaches the end of the table:

```c
RC insertRecord(RM_TableData *rel, Record *record) {
    int recordSize = getRecordSize(rel->schema);
    int numSlots = getRecordsPerPage(rel->schema);

    int curPageNum = 1;                 // data starts at page 1
    bool foundPage = false;

    while (!foundPage) {
        BM_PageHandle pageHandle;
        if (pinPage(rel->mgmtData, &pageHandle, curPageNum) != RC_OK)
            return RC_RM_BUFFER_PIN_FAILED;

        // Find the first free slot (marker != '+': never-used '\0' or tombstone '-')
        int slotNum = -1;
        for (int i = 0; i < numSlots; i++) {
            int offset = i * (recordSize + 2);
            if (pageHandle.data[offset] != '+') { slotNum = i; break; }
        }

        if (slotNum >= 0) {                                     // this page has room
            int offset = slotNum * (recordSize + 2);
            memcpy(pageHandle.data + offset + 1, record->data, recordSize);
            pageHandle.data[offset] = '+';                      // mark the slot occupied
            record->id.page = curPageNum;
            record->id.slot = slotNum;

            if (markDirty(rel->mgmtData, &pageHandle) != RC_OK)
                return RC_RM_MARK_DIRTY_FAILED;
            rel->numTuples++;                                   // bump the in-memory count
            if (unpinPage(rel->mgmtData, &pageHandle) != RC_OK)
                return RC_RM_BUFFER_UNPIN_FAILED;
            return RC_OK;
        }
        unpinPage(rel->mgmtData, &pageHandle);                  // full; try the next page
        curPageNum++;
    }
    return RC_OK;
}
```

Key points:

- **Page-full check by finding a free slot**: each page's `numSlots` markers are scanned; the first one that is not `'+'` is a free slot. Costs O(slots per page), but it is simple and easy to follow.
- **Insert position = first free slot**: the insert reuses the page's first free slot — whether a never-used `'\0'` slot or a tombstone `'-'` slot left by a delete. This never overwrites a live record and reclaims tombstone space (see the discussion in 3.4.4).
- **On-demand file growth**: `pinPage` calls `appendEmptyBlock` when `curPageNum` is past the end of the file, so the loop is guaranteed to terminate without manually tracking a "number of data pages".
- **RID is the return value**: on success, `record->id` is filled with `(page, slot)`, which the caller can pass to `getRecord` / `deleteRecord`.
- **`markDirty` + `unpin`**: only marks the page dirty and releases the pin; the actual flush is left to the buffer pool (`forceFlushPool` at close), not per-insert.

---

### 3.4.4 Deletion and tombstones: deleteRecord

Directly erasing a record from a page would break the `slot × (recordSize + 2)` offset formula (subsequent records would shift). This implementation uses the simplest form of **tombstone**: instead of removing the record bytes, it flips the slot's **marker byte** to `'-'`. Physically nothing is removed; logically the record is gone:

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
    if (marker != '+') {                        // slot already empty/deleted
        unpinPage(rel->mgmtData, &pageHandle);  // release the pin before returning
        return RC_RM_INVALID_RID;
    }

    pageHandle.data[offset] = '-';              // stamp the tombstone
    if (markDirty(rel->mgmtData, &pageHandle) != RC_OK)
        return RC_RM_MARK_DIRTY_FAILED;
    rel->numTuples--;                           // decrement the in-memory count
    if (unpinPage(rel->mgmtData, &pageHandle) != RC_OK)
        return RC_RM_BUFFER_UNPIN_FAILED;
    return RC_OK;
}
```

`getRecord` checks the marker byte first; anything other than `'+'` returns `RC_RM_INVALID_RID` (524) — reading a deleted or empty slot is an illegal operation:

```c
char marker = pageHandle.data[offset];
if (marker != '+') {
    unpinPage(rel->mgmtData, &pageHandle);      // release the pin before returning
    return RC_RM_INVALID_RID;
}
```

Scans (3.4.5) skip `'-'` slots too. This is the simplest "soft delete":

- **Pro**: dead-simple, doesn't disturb RID addressing — `RID = (page, slot)` means the same thing before and after a delete; a tombstone costs only 1 marker byte.
- **Con**: space is not fully reclaimed — the file never shrinks and emptied pages stay at the end; tombstone slots are reused on insert (see 3.4.3) but pages are never compacted, so repeated insert/delete cycles can still leave the file "bloated".

---

### 3.4.5 Linear scan: startScan / next / closeScan

A scan = page-by-page, slot-by-slot traversal with condition filtering. `startScan` packages the scan state (current page/slot, the condition expression, and the **table's total page count at scan start**) into an `RM_ScanInfo`:

```c
RC startScan(RM_TableData *rel, RM_ScanHandle *scan, Expr *cond) {
    RM_ScanInfo *scanInfo = (RM_ScanInfo *) malloc(sizeof(RM_ScanInfo));
    if (scanInfo == NULL) return RC_RM_MEM_ALLOC_FAILED;

    scanInfo->curPage = 1;            // data starts at page 1
    scanInfo->curSlot = -1;
    scanInfo->condition = cond;

    // Key: fix the table's total page count at scan start as the scan bound
    scanInfo->totalPages = getTotalNumPages((BM_BufferPool *) rel->mgmtData);

    scan->rel = rel;
    scan->mgmtData = scanInfo;
    return RC_OK;
}
```

`next` advances the scan and returns one matching record per call. It first checks the slot marker (skipping empty and tombstone slots), then uses `evalExpr` (implemented in `expr.c`) to evaluate the condition on a live record:

```c
RC next(RM_ScanHandle *scan, Record *record) {
    RM_ScanInfo *scanInfo = (RM_ScanInfo *) scan->mgmtData;
    int recordSize = getRecordSize(scan->rel->schema);
    int numSlots = getRecordsPerPage(scan->rel->schema);

    while (true) {
        // Bound by the totalPages captured at startScan. This must NOT be a live
        // getTotalNumPages() call: pinPage grows the file for pages that do not
        // exist yet, so a live bound would chase the growing file forever.
        if (scanInfo->curPage >= scanInfo->totalPages)
            return RC_RM_NO_MORE_TUPLES;

        scanInfo->curSlot++;

        if (scanInfo->curSlot >= numSlots) {   // current page done; next page
            scanInfo->curPage++;
            scanInfo->curSlot = 0;
            continue;
        }

        BM_PageHandle pageHandle;
        if (pinPage(scan->rel->mgmtData, &pageHandle, scanInfo->curPage) != RC_OK)
            return RC_RM_BUFFER_PIN_FAILED;

        int offset = scanInfo->curSlot * (recordSize + 2);
        char marker = pageHandle.data[offset];
        if (marker != '+') {                   // empty/tombstone slot; skip
            unpinPage(scan->rel->mgmtData, &pageHandle);
            continue;
        }

        memcpy(record->data, pageHandle.data + offset + 1, recordSize);
        record->id.page = scanInfo->curPage;
        record->id.slot = scanInfo->curSlot;

        if (scanInfo->condition == NULL) {     // no condition; return directly
            unpinPage(scan->rel->mgmtData, &pageHandle);
            return RC_OK;
        }

        // Evaluate the condition; return the record only if it matches
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

`closeScan` frees the `RM_ScanInfo`:

```c
RC closeScan(RM_ScanHandle *scan) {
    free(scan->mgmtData);
    scan->mgmtData = NULL;
    return RC_OK;
}
```

Key points:

- **The scan bound is the easiest pitfall in this step**: it must be the `totalPages` captured at `startScan`. If you instead call the live `getTotalNumPages()` each iteration, the bound grows together with the file (because `pinPage` calls `appendEmptyBlock` for pages that do not exist yet) and the scan never terminates.
- **One pin/unpin per slot**: the scan pins a page, reads a slot, unpins — simple but with buffer-pool overhead per slot; fine at course scale. Chapter 4 replaces "scan the whole table" with "walk the index".
- **Tombstones are skipped early**: `'-'` slots are filtered at the marker check, so they never reach `evalExpr`.
- The whole scan costs **O(all records in the table)** — no index, pure linear.

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
| `insertRecord(rel, record)` | Insert | Walk pages for room → write → backfill RID |
| `deleteRecord(rel, id)` | Delete | Set slot marker to `'-'` (tombstone) |
| `updateRecord(rel, record)` | Update | Locate by RID, overwrite the whole record |
| `getRecord(rel, id, record)` | Point query | Read one record by `(page, slot)` |
| `startScan(rel, scan, cond)` | Start scan | Initialize `RM_ScanInfo` (with a fixed `totalPages`) |
| `next(scan, record)` | Next | Page-by-page + marker filtering + `evalExpr` |
| `closeScan(scan)` | End scan | Free `RM_ScanInfo` |
| `getRecordSize(schema)` | Size | Sum of column lengths |
| `createSchema(...)` / `freeSchema` | Schema lifecycle | — |
| `createRecord(...)` / `freeRecord` | Record lifecycle | — |
| `getAttr` / `setAttr` | Attr read/write | Get/set one column by offset |

This implementation persists `numTuples` **directly** on page 0: `closeTable`
writes it back before shutting the pool down, and `openTable` reads it back into
`rel->numTuples` — so the count survives a close/reopen round trip.

The tombstone is a marker byte (`'-'`) in the slot header, separate from record
data, so valid records are never misjudged. A production layout would go further
and use a slot-state bitmap or tuple-header flags to support more slot states.

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

1. **The tombstone in this implementation is a marker byte `'-'` in the slot header, separate from record data, so valid records are never misjudged. But if you needed more slot states (unused / occupied / deleted / updated), is 1 marker byte enough? How would you encode slot states?** Hint: a slot-state bitmap, or more values for the marker byte.

2. **`insertRecord` now scans the page for the first non-`'+'` slot (reusing tombstone/empty slots). For tables with frequent "delete-then-insert" patterns, "restarting the scan at slot 0 on every insert" is still a cost. How could you avoid rescanning?** Hint: an in-page free-slot list, or a "free hint" in the page header pointing at the next likely-free index.

3. **The linear scan's bound is the `totalPages` fixed at `startScan`. What would happen if you changed it to call `getTotalNumPages()` every iteration inside `next`? Why?** Hint: what does `pinPage` do for a page that does not exist yet (Chapter 2)?

---

> **Next chapter**: [Chapter 4 · B+ Tree Index](04-btree-index.en.md)
