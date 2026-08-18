# Chapter 6 · End-to-End Integration

> Source file: `demo_api.c`
>
> The first five chapters are each self-contained — storage, buffer pool, record management, B+ tree index, DDL parser. This chapter **chains them into a complete call path**, demonstrating the workflow of a real database engine: create table → insert data → index lookup → range scan.

---

## 6.1 Why This Chapter

Each layer is simple in isolation, but the most common student question is: *how do these layers cooperate? What exactly does a `CREATE TABLE` trigger? Is the index maintained together with the table, or separately?*

`demo_api.c` is the minimal runnable sample that answers these. It introduces no new mechanisms — it simply calls the APIs from the previous five chapters in the order a real engine would: DDL creates the table and index files, we open both handles, every insert writes to the table **and** the index, and finally we use the index for a point lookup and a range scan. Read this demo and you understand the skeleton of a relational storage engine.

---

## 6.2 demo_api.c Flow Overview

`demo_api.c`'s `main` has 7 steps — a full "create → use → drop" lifecycle:

```
┌──────────────────────────────────────────────────────────────────┐
│  1. initRecordManager / initIndexManager    ← init subsystems     │
│  2. executeDDL("CREATE TABLE users ...")     ← DDL creates table  │
│  3. openTable + openBTree                    ← get two handles    │
│  4. insertUser × 5                           ← insert + maintain  │
│  5. findKey + getRecord                      ← index point lookup │
│  6. openTreeScan + nextEntry loop            ← index range scan   │
│  7. closeBTree / closeTable / DROP TABLE     ← cleanup            │
└──────────────────────────────────────────────────────────────────┘
```

Key insight: **the table and the index are two independent storage objects** (`USERS` table file + `USERS.idx` index file). The caller is responsible for keeping them consistent — `insertUser` first calls `insertRecord`, then `insertKey`, storing the record's RID as the value side of the B+ tree entry.

---

## 6.3 Code Walkthrough

### 6.3.1 `insertUser`: Double-Write Helper

This is the core helper of the demo: build a record, write it to the table, then write `(key → RID)` into the index:

```c
static RC insertUser(RM_TableData *tbl, BTreeHandle *idx,
                     int id, const char *name, int age)
{
    Schema *sch = tbl->schema;
    Record *r;
    createRecord(&r, sch);

    Value *v;
    MAKE_VALUE(v, DT_INT, id);      setAttr(r, sch, 0, v);  freeVal(v);
    MAKE_STRING_VALUE(v, name);     setAttr(r, sch, 1, v);  freeVal(v);
    MAKE_VALUE(v, DT_INT, age);     setAttr(r, sch, 2, v);  freeVal(v);

    /* 1. insert into the table (assigns r->id = RID) */
    insertRecord(tbl, r);

    /* 2. insert (id -> RID) into the primary-key index */
    Value *key; MAKE_VALUE(key, DT_INT, id);
    insertKey(idx, key, r->id);
    freeVal(key);

    freeRecord(r);
    return RC_OK;
}
```

The order matters: `insertRecord` must come first because it assigns the RID (`r->id` = `page.slot`), and that RID is the value stored in the index entry. Reverse the order and you'd have no RID to put in the tree.

---

### 6.3.2 Point Lookup: `findKey` + `getRecord`

"Find user with id=20" is two steps: translate the key to a RID via the B+ tree, then fetch the record by RID:

```c
Value *q; MAKE_VALUE(q, DT_INT, 20);
RID rid;
if (findKey(idx, q, &rid) == RC_OK) {
    Record *r;
    createRecord(&r, tbl.schema);
    getRecord(&tbl, rid, r);
    char *s = serializeRecord(r, tbl.schema);
    printf("   found at [%d-%d]: %s\n", rid.page, rid.slot, s);
    free(s);
    freeRecord(r);
}
freeVal(q);
```

`findKey` walks the B+ tree root-to-leaf in O(log n); `getRecord` uses `rid.page` to find the page (via the buffer pool) and `rid.slot` to locate the record within it. This is the textbook implementation of a primary-key equality query.

---

### 6.3.3 Range Scan: `openTreeScan` + `nextEntry`

"List all users ordered by id" uses an index scan:

```c
BT_ScanHandle *sc = NULL;
openTreeScan(idx, &sc);
RID cur;
Record *r;
createRecord(&r, tbl.schema);
while (nextEntry(sc, &cur) == RC_OK) {
    getRecord(&tbl, cur, r);
    char *s = serializeRecord(r, tbl.schema);
    printf("   %s\n", s);
    free(s);
}
freeRecord(r);
closeTreeScan(&sc);
```

`openTreeScan` positions the cursor at the first entry of the leftmost leaf; each `nextEntry` follows the **leaf-level sibling pointer** to the right, emitting RIDs in ascending key order. This is exactly why B+ tree leaves are linked into a list — to support range scans without ever climbing back to internal nodes.

---

## 6.4 Index Scan vs Linear Scan

This is the most important comparison in the chapter. For "list all users ordered by id" there are two implementations:

- **Linear scan**: use `RM_ScanHandle` to walk page 0 slot 0 onward, page by page, slot by slot, collect every record's id, then sort. Cost: O(n) record reads + O(n log n) sort.
- **Index scan**: use `BT_ScanHandle` to walk the leaf chain. Leaves are already sorted by key and pack many keys per page, so you only touch leaves: O(#leaves) ≈ O(n / leaf fanout).

| Dimension | Linear scan `startScan` | Index scan `openTreeScan` |
|-----------|-------------------------|---------------------------|
| Source | every page/slot of the table file | B+ tree leaf nodes |
| Order | unordered, needs external sort | naturally ascending |
| Read amplification | reads every record | follows leaf-chain pointers |
| Complexity | O(n) I/O | O(#leaves) ≈ O(n / F) I/O |
| Use case | full-table stats, non-indexed columns | range/sort on indexed column |

Why is the index scan faster? The key is the **`next` pointer between B+ tree leaves** (see `btree_mgr.c:14`). A range scan never climbs back to internal nodes — it just walks right along the leaf chain. A linear scan, by contrast, must traverse every page of the table (including empty/deleted slots) and then sort the unordered result.

---

## 6.5 Expected Output

Running `./build/demo_api` produces:

```
== 1. executeDDL: CREATE TABLE users ==
== 2. open table + index ==
== 3. insert 5 users ==
   table tuples: 5,  index entries: 5
== 4. index lookup: WHERE id = 20 ==
   found at [1-1]: (20,dave,28)
== 5. index scan: all users in id order ==
   (10,bob,30)
   (20,dave,28)
   (30,alice,25)
   (40,eve,35)
   (50,carol,22)
== 6. cleanup ==
== done ==
```

Note the insertion order is **30, 10, 50, 20, 40** (out of order), but step 5 returns them strictly as **10, 20, 30, 40, 50**. This is the core promise of a B+ tree: **sorting is maintained at insert time; ordered output is free at query time.** The RID `[1-1]` means record id=20 lives at page 1, slot 1 of the table file — page/slot are assigned by the record manager and are unrelated to the key.

---

## 6.6 Architecture Review: Call Chain

Tracing each demo_api.c API call all the way down to disk I/O:

```
demo_api.c call                    layers traversed              hits disk
─────────────────────────────────────────────────────────────────────
executeDDL("CREATE TABLE..")  → ddl_parser → record_mgr    → buffer_mgr → storage_mgr → file create
openTable / openBTree         → record_mgr / btree_mgr     → buffer_mgr → storage_mgr → read header page
insertRecord                  → record_mgr                 → buffer_mgr → storage_mgr → page write
insertKey                     → btree_mgr                  → buffer_mgr → storage_mgr → page write
findKey                       → btree_mgr                  → buffer_mgr → storage_mgr → page read
getRecord                     → record_mgr                 → buffer_mgr → storage_mgr → page read
openTreeScan / nextEntry      → btree_mgr (leaf chain)     → buffer_mgr → storage_mgr → page read
```

The main data path has a clear boundary: `record_mgr` does not call `fread` directly; it goes through `buffer_mgr`, which calls `storage_mgr.readBlock` on a miss. This is not a strict linear stack: `btree_mgr` and `record_mgr` are sibling consumers of page services, and the DDL/DML executor composes several modules. Replacement is transparent only while public interfaces and semantics remain compatible.

---

## 6.7 Known Limitations and Extensions

`demo_api.c` (and the engine behind it) deliberately simplifies things, leaving room for extension:

1. **The low-level C API does not maintain indexes automatically**: after `insertRecord`, `demo_api.c` must call `insertKey`. The SQL path's `query_executor` maintains the primary-key index, but direct record-API use can still create divergence. Production systems update table and indexes atomically in a unified execution/storage transaction path.
2. **`deleteKey` does not rebalance**: deletion only removes the key from its leaf — no borrow, no merge (see the comment at `btree_mgr.c:11`). The tree stays correct but may become under-full. This is the standard CS525 simplification.
3. **This demo does not use DML**: `demo_api.c` calls the C API directly. Project-level DML support is provided by Chapter 7's `dml_parser`, `query_executor`, and `demo_sql.c`.
4. **Single-threaded, single-connection**: no transactions, no locks, no concurrency control.
5. **No query optimizer**: index scan vs. linear scan is currently chosen by the caller.

Extensions to consider: ① make table/index updates atomic; ② implement borrow/merge in `deleteKey`; ③ extend the limited DML grammar with joins and ordering; ④ add a cost-based optimizer that picks between index scan and linear scan automatically.

---

## 6.8 Exercises

1. **Why must `insertUser` call `insertRecord` first and then `insertKey`, not the other way around?** If reversed, what RID would the index store? Hint: when is `r->id` assigned?

2. **Suppose the `users` table has 1,000,000 records, 100 per page, and the B+ tree leaf fanout F=200. For `SELECT * FROM users ORDER BY id`, roughly how many page I/Os would a linear scan and an index scan each need?** Hint: linear scan reads all data pages and sorts; index scan only walks the leaf chain.

3. **The current `deleteKey` doesn't rebalance. If you deleted 900,000 keys in a row from a B+ tree of 1,000,000 keys, how would query performance degrade? Would the tree height change?** Hint: underflow doesn't change height, but leaf utilization drops.

---

> **Full source**: see [demo_api.c](../demo_api.c). Back to [README](../README.md) for the project overview.
