# Chapter 2 · Buffer Manager

> Source files: `buffer_mgr.c` / `buffer_mgr.h`
>
> Chapter 1 solved "how data lands on disk". But going to disk on every page access is far too slow — this chapter inserts a layer of **in-memory cache** between disk and upper layers to keep hot pages in memory.

---

## 2.1 Why this layer is needed

A random disk access takes milliseconds (HDD) to hundreds of microseconds (SSD); a memory access takes nanoseconds — a 10⁵–10⁶× gap. If every page access went straight to `readBlock`, the database would be unusable.

Worse, **temporal/spatial locality** means the same record is touched repeatedly and neighbouring records are scanned together. Re-reading the disk every time is pure waste.

The buffer manager is the layer between the storage manager and upper layers: it keeps a fixed-size **buffer pool** in memory caching recently used pages. On a read, it first checks whether the page is already in memory (**hit**); only on a **miss** does it go to disk. Writes are similar: modify the in-memory copy, mark it **dirty**, and flush it back to disk later.

---

## 2.2 Core principle: the three pieces of a buffer pool

A buffer pool is several pieces working together in memory:

```
┌─────────────────────────────────────────────────────────────┐
│  buffPoolAddr    :  numPages × 4096 bytes of page copies   │
│  buffPoolHeaders :  per-slot metadata (pageNum/dirty/pin)  │
│  buffTable       :  pageNum → buff_id hash map              │
│  freeBuffList    :  list of never-used slots                │
│  strategyData    :  FIFO/LRU access-order list               │
└─────────────────────────────────────────────────────────────┘
```

- **Page-copy array**: a contiguous `numPages × PAGE_SIZE` block; slot `i` starts at `buffPoolAddr + i × 4096`.
- **Hash table**: maps `pageNum` → `buff_id` in O(1). Without it we'd scan `buffPoolHeaders` in O(numPages).
- **Strategy list**: records access order; when no free slot is left, the strategy picks a victim.

`pinPage` is exactly these pieces working together: **look up the hash table → on hit, update the strategy list and return; on miss, take a free slot (or evict a victim per the strategy) → read the disk → insert `pageNum→buff_id` into the hash table**.

**pin/unpin lifecycle**: before using a page, the upper layer must `pinPage` (nail it in the pool so it can't be evicted); after use it must `unpinPage`. Why must pin? If A just got a page and B then evicted it, A's pointer would point to someone else's data — a classic dangling reference. `fixCount` records how many users hold a pin on the page; only when it drops to 0 may the page be replaced. Modified content must be `markDirty`'d, and the page is written back to disk before being evicted.

---

## 2.3 Key data structures

> **⚠️ Implementation-difference note (after the 2026-08 swap to the cs525-s23-11 implementation)**
> The pinPage flow in 2.2, the data structures in 2.3, and the walkthrough in 2.5 all describe the **classic textbook design**: `BM_MgmtData` + hash table + free list + `isOpen`.
> The **actual code** in `src/buffer_mgr.c` now uses a simpler design:
> - a **doubly linked list of `BufferFrame`s** (each with dirty / fixCount / entryTimestamp / accessTimestamp), pointed to by `bm->mgmtData`;
> - **linear list lookup**, no hash table (the pool is small, so a linear walk is simple and readable);
> - only **FIFO (by entryTimestamp)** and **LRU (by accessTimestamp)** are implemented; other strategies return -1;
> - `pinPage` grows the file on demand via `appendEmptyBlock` for pages that do not exist yet.
>
> The public API (§2.6) is unchanged, so upper layers call it the same way. The classic design below (hash table, load factor, LRU-K) is still worth reading as teaching material; to check the real code, see the file-header comment in `src/buffer_mgr.c`.

The public handle `BM_BufferPool` exposes only interface-level fields; the real implementation hides inside `mgmtData`:

```c
typedef struct BM_BufferPool {
    char *pageFile;
    int   numPages;
    bool  isOpen;
    ReplacementStrategy strategy;
    BM_MgmtData *mgmtData;
} BM_BufferPool;
```

`BM_MgmtData` is the actual "body" of the pool:

```c
typedef struct BM_MgmtData {
    SM_FileHandle *fHandle;
    char *buffPoolAddr;
    BufferHeader *buffPoolHeaders;
    BufferStats buffStats;
    HashTable *buffTable;
    FreeList *freeBuffList;
    int *fixCount;
    void *strategyData;
} BM_MgmtData;
```

Per-slot metadata:

```c
typedef struct BM_BufferHeader {
    unsigned int buff_id;
    PageNumber pageNumber;
    bool dirtyPage;
    bool pinned;
} BufferHeader;
```

Note that `BufferHeader` does **not** hold the page data itself — data lives at `buffPoolAddr[buff_id × PAGE_SIZE]`. Separating headers from data keeps the headers compact and cache-friendly when scanning metadata.

**Hash table — why not linear search?** The pool is typically tens to hundreds of pages, which sounds small; but `pinPage` is one of the hottest functions in the database — every record access goes through it. O(numPages) linear scan vs O(1) hashing makes a real difference under load. The hash function is the **multiplication method** from CLRS Ch. 11:

```c
size_t hashOfKey(HashTable *hTable, int key) {
    double a = (sqrt(5) - 1) / 2;                  // golden ratio fractional part ≈ 0.618
    int wSize = 32;
    unsigned int s = (unsigned) floor(a * 2 * ((unsigned int) 2 << (wSize - 2)));
    unsigned int x = key * s;                       // mix high bits
    int p = (int) (log(hTable->size) / log(2));
    size_t hashValue = x >> (wSize - p);           // take top p bits
    if (hashValue > hTable->size) hashValue %= hTable->size;
    return hashValue;
}
```

Benefits of the multiplication method: ①uniform distribution for any `key`, few collisions; ②independent of `key`'s distribution (page numbers tend to be contiguous, which hurts the division method); ③only needs the table size to be a power of two — taking the top bits is a shift, no division, fast. `initBufferPool` sets the table size to `2 × numPages`, keeping the load factor around 0.5 so chains stay short and lookups stay fast.

---

## 2.4 Replacement strategies: FIFO / LRU / LRU-K

Once the pool is full, who gets evicted? That's the replacement strategy's question. This implementation uses a single list `strategyData` to record access order; the three strategies differ only in *when* a slot is moved to the tail:

- **FIFO**: a page goes to the tail **when it first enters the pool**. Eviction takes from the head — earliest in, first out. It ignores later accesses entirely.
- **LRU**: every **hit** also moves the slot to the tail. So the head is always the "least recently used" page — FIFO, but aware of re-access.
- **LRU-K**: a refinement that looks at the timestamps of the **last K** accesses, so a one-off scan doesn't evict a hot page. Here `RS_LRU_K` does the same `shiftNodeById` on hit — a simplified variant.

In code it's just two snippets:

```c
// On hit: LRU / LRU_K move this page to the tail; FIFO does nothing
if (bm->strategy == RS_LRU || bm->strategy == RS_LRU_K) {
    shiftNodeById(bm->mgmtData->strategyData, buffId);
}
```

```c
// On eviction: take the first non-pinned slot from the head, move to tail
node = getListHead(bm->mgmtData->strategyData);
while (node) {
    buffId = node->buff_id;
    if (bm->mgmtData->buffPoolHeaders[buffId].pinned
        || bm->mgmtData->fixCount[buffId] > 0) {
        node = node->next;                        // skip pinned
    } else {
        shiftToEnd(bm->mgmtData->strategyData, node);
        break;
    }
}
```

Note that all strategies **skip pages with `pinned=TRUE` or `fixCount>0`** — a pinned page must never be evicted.

---

## 2.5 Key code walkthrough

### 2.5.1 `pinPage`: lookup → hit/miss → replacement → disk read

`pinPage` is the heart of the module. Its main trunk (boundary checks and stats omitted):

```c
RC pinPage(BM_BufferPool *const bm, BM_PageHandle *const page,
           const PageNumber pageNum) {
    HashTable *hTable = bm->mgmtData->buffTable;
    int buffId = searchHashTable(hTable, pageNum);   // ① lookup

    if (buffId < 0) {                                // ② miss
        node = getFreeNode(bm->mgmtData->freeBuffList);
        if (node == NULL) {                          // no free slot → evict
            node = getListHead(bm->mgmtData->strategyData);
            while (node) {
                buffId = node->buff_id;
                if (buffPoolHeaders[buffId].pinned || fixCount[buffId] > 0)
                    node = node->next;               // skip pinned
                else { shiftToEnd(strategyData, node); break; }
            }
            if (buffPoolHeaders[buffId].dirtyPage)   // flush if dirty
                forcePage(bm, pHand);
        } else {                                     // free slot: use it
            appendByNode(strategyData, node);
            buffId = node->buff_id;
        }

        ensureCapacity(pageNum + 1, fHandle);        // ③ grow file if needed
        readBlock(pageNum, fHandle,                  // ④ read disk
                  &buffPool[buffId * PAGE_SIZE]);
        replaceHashNode(buffTable,                    // ⑤ update mapping
                        oldPageNum, pageNum, buffId);
        num_reads_disk++;
    } else {                                         // ② hit
        if (strategy == RS_LRU || strategy == RS_LRU_K)
            shiftNodeById(strategyData, buffId);
        num_buff_hits++;
    }

    buffPoolHeaders[buffId].pinned = TRUE;           // ⑥ pin + fixCount++
    buffPoolHeaders[buffId].pageNumber = pageNum;
    fixCount[buffId]++;
    page->pageNum = pageNum;                         // ⑦ return to caller
    page->data = &buffPool[buffId * PAGE_SIZE];
    return RC_OK;
}
```

Seven steps: ① look up `pageNum` → `buffId` in the hash table; ② on hit, move it for LRU; on miss, continue; ③ `ensureCapacity` grows the file if the page doesn't exist yet (lazy growth from Ch.1); ④ `readBlock` loads the page into the slot; ⑤ replace the old `pageNum` mapping with the new one in the hash table; ⑥ pin the slot, `fixCount++`; ⑦ expose the in-memory pointer via `page->data` — the caller reads/writes this page directly through this pointer, zero-copy.

---

### 2.5.2 `unpinPage` and `markDirty`

`unpinPage` mirrors `pinPage` — it just decrements `fixCount`:

```c
RC unpinPage(BM_BufferPool *const bm, BM_PageHandle *const page) {
    int buffId = searchHashTable(bm->mgmtData->buffTable, page->pageNum);
    if (buffId < 0) return RC_UNPIN_FAILED;
    bm->mgmtData->buffPoolHeaders[buffId].pinned = FALSE;
    if (bm->mgmtData->fixCount[buffId] > 0)
        bm->mgmtData->fixCount[buffId] -= 1;
    return RC_OK;
}
```

Note `pinned` is set to `FALSE` immediately, but `fixCount` is only **decremented by 1** — it may not reach zero, since the same page can be pinned by multiple callers at once. Only when the last one unpins, `fixCount==0` and the page becomes evictable. This is the classic **reference counting** pattern.

`markDirty` is equally light — just flag the slot:

```c
RC markDirty(BM_BufferPool *const bm, BM_PageHandle *const page) {
    int buffId = searchHashTable(bm->mgmtData->buffTable, page->pageNum);
    if (buffId < 0) return RC_DIRTY_FAILED;
    bm->mgmtData->buffPoolHeaders[buffId].dirtyPage = TRUE;
    return RC_OK;
}
```

Why not write back immediately? A disk write costs milliseconds; the caller may modify the same page many times, and writing back each time would be I/O-bound. **Deferred write** is one of the buffer pool's key optimizations.

---

### 2.5.3 `forceFlushPool`: dirty page writeback

`forceFlushPool` writes all dirty pages back to disk. `shutdownBufferPool` calls it before tearing down a pool to prevent data loss:

```c
RC forceFlushPool(BM_BufferPool *const bm) {
    for (i = 0; i < bm->numPages; ++i) {
        if (bm->mgmtData->buffPoolHeaders[i].dirtyPage) {  // scan all slots
            pHandle->pageNum = bm->mgmtData->buffPoolHeaders[i].pageNumber;
            forcePage(bm, pHandle);                       // write each back
        }
    }
    return RC_OK;
}
```

`forcePage` does one concrete thing: `memcpy` the `buff_id` slot out, call Ch.1's `writeBlock` to write it to the matching page number on disk, clear the `dirtyPage` flag (only when `fixCount==0`), and bump `num_writes_disk`. Note it only clears the dirty flag when `fixCount==0` — if someone still has the page pinned, writing it back doesn't make it "clean", because they may modify it again right away.

---

## 2.6 Complete API overview

| Function | Purpose | One-liner |
|----------|---------|-----------|
| `initBufferPool(bm, name, n, strat, data)` | Initialize | Open page file, allocate n slots, build hash table and strategy list |
| `shutdownBufferPool(bm)` | Shutdown | Flush all dirty pages, check no pins, free all memory |
| `forceFlushPool(bm)` | Writeback | Flush all dirty pages to disk at once |
| `markDirty(bm, page)` | Mark dirty | Set dirty flag on a page |
| `unpinPage(bm, page)` | Unpin | `fixCount--`, allow eviction |
| `forcePage(bm, page)` | Write single page | Write one page back to disk |
| `pinPage(bm, page, pageNum)` | Pin page | Hit/miss/replace/read + `fixCount++` |
| `getFrameContents(bm)` | Metadata | Array of pageNums per slot |
| `getDirtyFlags(bm)` | Metadata | Array of dirty flags per slot |
| `getFixCounts(bm)` | Metadata | Array of fixCount per slot |
| `getNumReadIO(bm)` | Stats | Cumulative disk reads |
| `getNumWriteIO(bm)` | Stats | Cumulative disk writes |

---

## 2.7 Compilation and verification

```bash
# Build the whole project
make all

# No standalone buffer-manager target exists; this regression test covers it indirectly
./build/test_assign3_1
```

The regression test exercises page-file creation, `pinPage`, `markDirty`, `unpinPage`, and flushing through the record manager. It is not a standalone unit test of replacement-policy behavior.

To test manually:

```c
BM_BufferPool bm;
BM_PageHandle page;
initBufferPool(&bm, "test.bin", 5, RS_LRU, NULL);

pinPage(&bm, &page, 0);    // miss → disk read
pinPage(&bm, &page, 1);    // miss → disk read
pinPage(&bm, &page, 0);    // hit → num_buff_hits++ (no getter exposed)
markDirty(&bm, &page);
unpinPage(&bm, &page);

printf("reads = %d\n", getNumReadIO(&bm));     // 2
printf("writes = %d\n", getNumWriteIO(&bm));    // 0 (not flushed yet)

shutdownBufferPool(&bm);    // closing the pool auto-flushes dirty pages
```

---

## 2.8 Discussion questions

1. **`pinned` is a bool and `fixCount` is an int — are they redundant? Could you use only `fixCount>0` to decide whether a page can be evicted?** Hint: look at the order of `pinned=FALSE` and `fixCount--` in `unpinPage`, and the semantics when multiple callers pin the same page.

2. **Under FIFO, if a page is constantly pinned/unpinned but `fixCount` stays > 0 (always pinned), does its position in the `strategyData` list change? What problem does this cause?** Hint: FIFO doesn't move nodes on hit; consider the disconnect between "page residency" and FIFO queue order.

3. **`shutdownBufferPool` checks "is any page still pinned?" after flushing dirty pages before it can close the pool. Why not just force-unpin everything and close?** Hint: who owns the `page->data` pointer? What would the upper layer get if you forcibly released?

---

> **Next chapter**: [Chapter 3 · Record Manager](03-record-manager.en.md)
