# 第2章 · 缓冲池管理器 Buffer Manager

> 对应源文件：`buffer_mgr.c` / `buffer_mgr.h`
>
> 第1章解决了「数据怎么落在磁盘上」的问题。但每次读页都直接读写磁盘太慢了——这一章在磁盘和上层之间加一层**内存缓存**，把热点页留在内存里。

---

## 2.1 为什么需要这一层

**中文**

磁盘随机读写的延迟在毫秒级（HDD）到百微秒级（SSD），而内存访问只要纳秒级——差了 10⁵ 到 10⁶ 倍。如果上层每次想看一页都直接 `readBlock`，数据库就基本没法用了。

更糟糕的是**时间/空间局部性**：同一条记录会被多次访问、相邻记录会被一起扫描。如果每次都重新读盘，这种重复访问的成本完全浪费了。

缓冲池管理器（buffer manager）就是夹在存储管理器和上层之间的中间层：它在内存里维护一块固定大小的**缓冲池**，把最近用过的页缓存起来。上层要读一页时，先看它在不在内存里——在就直接返回（**命中 hit**），不在才去磁盘读（**未命中 miss**）。写也是类似：先改内存里的副本，标记为「脏」（dirty），稍后再统一写回磁盘。

**English**

A random disk access takes milliseconds (HDD) to hundreds of microseconds (SSD); a memory access takes nanoseconds — a 10⁵–10⁶× gap. If every page access went straight to `readBlock`, the database would be unusable.

Worse, **temporal/spatial locality** means the same record is touched repeatedly and neighbouring records are scanned together. Re-reading the disk every time is pure waste.

The buffer manager is the layer between the storage manager and upper layers: it keeps a fixed-size **buffer pool** in memory caching recently used pages. On a read, it first checks whether the page is already in memory (**hit**); only on a **miss** does it go to disk. Writes are similar: modify the in-memory copy, mark it **dirty**, and flush it back to disk later.

---

## 2.2 核心原理：缓冲池的三件套

**中文**

一个缓冲池在内存里有几块东西配合工作：

```
┌─────────────────────────────────────────────────────────────┐
│  buffPoolAddr    :  numPages × 4096 字节的页副本数组       │
│  buffPoolHeaders :  每个槽位的元信息(页号/dirty/pin)       │
│  buffTable       :  pageNum → buff_id 的哈希映射           │
│  freeBuffList    :  还没用过的空槽位链表                   │
│  strategyData   :  FIFO/LRU 的访问顺序链表                 │
└─────────────────────────────────────────────────────────────┘
```

- **页副本数组**：连续 `numPages × PAGE_SIZE` 字节，第 `i` 个槽位的起始地址 = `buffPoolAddr + i × 4096`。
- **哈希表**：给定 `pageNum`，O(1) 找到它在第几个槽位（`buff_id`）。没有它就要扫一遍 `buffPoolHeaders`，O(numPages)。
- **策略链表**：记录「使用顺序」。槽位不够时，按策略挑一个被换出去。

`pinPage` 的逻辑就是这三件套的联动：**查哈希表 → 命中就更新策略链表返回；未命中就从 freeBuffList 拿一个空槽（或按策略踢掉一个旧页）→ 读盘填进去 → 在哈希表登记 `pageNum→buff_id`**。

**pin/unpin 生命周期**：上层用一页前必须 `pinPage`（把页「钉」在缓冲池里，不能被替换出去），用完必须 `unpinPage`。为什么必须 pin？如果 A 刚拿到页准备读，B 这时把这一页换出去了，A 手里的指针就指向了别人的数据——典型的悬挂引用。`fixCount` 记录一页被几个人钉着，必须降到 0 才允许被替换。修改过页内容要 `markDirty`，被换出前会写回磁盘。

**English**

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

## 2.3 关键数据结构

**中文**

公开的句柄 `BM_BufferPool` 只放对外接口需要的字段，真正的实现细节藏在 `mgmtData` 里：

```c
typedef struct BM_BufferPool {
    char *pageFile;               // 底层 page file 名
    int   numPages;               // 缓冲池槽位数
    bool  isOpen;                 // 是否已初始化
    ReplacementStrategy strategy; // FIFO / LRU / LRU_K
    BM_MgmtData *mgmtData;        // 真正的实现，对外不透明
} BM_BufferPool;
```

`BM_MgmtData` 才是缓冲池的「肉体」：

```c
typedef struct BM_MgmtData {
    SM_FileHandle *fHandle;        // 底层 page file 句柄
    char *buffPoolAddr;            // 页副本数组起始地址
    BufferHeader *buffPoolHeaders; // 每个槽位的元信息
    BufferStats buffStats;         // 命中/读盘/写盘计数
    HashTable *buffTable;          // pageNum → buff_id
    FreeList *freeBuffList;        // 空闲槽位链表
    int *fixCount;                 // 每个槽位的 pin 计数
    void *strategyData;            // 替换策略链表（复用 FreeList）
} BM_MgmtData;
```

每个槽位的元信息：

```c
typedef struct BM_BufferHeader {
    unsigned int buff_id;   // 槽位编号 0..numPages-1
    PageNumber pageNumber;   // 当前缓存的页号，空槽为 NO_PAGE
    bool dirtyPage;          // 是否被改过还没写回
    bool pinned;             // 当前是否被钉住
} BufferHeader;
```

注意 `BufferHeader` 里**没有**存页数据本身——数据在 `buffPoolAddr[buff_id × PAGE_SIZE]`。页头和数据分离存放，可以让页头集中、紧凑，扫描元信息时缓存友好。

**哈希表——为什么不线性查找？** 缓冲池默认几十到几百页，看似不大；但 `pinPage` 是数据库热路径上调用最频繁的函数之一，每条记录访问都要走一次。O(numPages) 的线性扫 vs O(1) 的哈希表，在压力下差距很大。哈希表用的是 CLRS 第 11 章的**乘法散列法**：

```c
size_t hashOfKey(HashTable *hTable, int key) {
    double a = (sqrt(5) - 1) / 2;                  // 黄金比例小数部分 ≈ 0.618
    int wSize = 32;
    unsigned int s = (unsigned) floor(a * 2 * ((unsigned int) 2 << (wSize - 2)));
    unsigned int x = key * s;                      // 高位混合
    int p = (int) (log(hTable->size) / log(2));    // p = log2(size)
    size_t hashValue = x >> (wSize - p);           // 取高 p 位
    if (hashValue > hTable->size) hashValue %= hTable->size; // 防越界
    return hashValue;
}
```

乘法散列的好处：①对任意 `key` 分布均匀，碰撞少；②不依赖 `key` 的分布（页号往往很连续，除留余数法容易撞）；③表大小只要是 2 的幂，取高位用移位实现，没有除法，快。`initBufferPool` 里把表大小设成 `2 × numPages`，装填因子维持在 0.5 左右，链表短，查得快。

**English**

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

## 2.4 替换策略：FIFO / LRU / LRU-K

**中文**

缓冲池满了之后踢谁？这就是替换策略要回答的问题。本实现统一用一条链表 `strategyData` 记录使用顺序，三种策略的差别只在「什么时候把一个槽位移到链表尾」：

- **FIFO**：页**首次进入缓冲池**时插到链表尾。换页时从头取——最早进来的最先走。完全不看之后有没有再被访问过。
- **LRU**：每次**命中**也把对应槽位移到链表尾。这样链表头永远是「最久没被碰过」的页，是 FIFO 的「考虑再访问」改进版。
- **LRU-K**：LRU 的进一步细化，看**最近 K 次**访问的时间戳来排序，避免一次偶发扫描把热点页冲掉。本实现的 `RS_LRU_K` 在命中时同样 `shiftNodeById`，是简化版处理。

代码里的体现就两处：

```c
// 命中时：LRU / LRU_K 把这一页移到链表尾；FIFO 不动
if (bm->strategy == RS_LRU || bm->strategy == RS_LRU_K) {
    shiftNodeById(bm->mgmtData->strategyData, buffId);
}
```

```c
// 换页时：从链表头找第一个没被 pin 住的，移到尾
node = getListHead(bm->mgmtData->strategyData);
while (node) {
    buffId = node->buff_id;
    if (bm->mgmtData->buffPoolHeaders[buffId].pinned
        || bm->mgmtData->fixCount[buffId] > 0) {
        node = node->next;                        // 跳过被钉住的
    } else {
        shiftToEnd(bm->mgmtData->strategyData, node);
        break;
    }
}
```

注意所有策略都**跳过 `pinned=TRUE` 或 `fixCount>0` 的页**——被钉住的页绝不能换出去。

**English**

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

## 2.5 关键代码逐行讲

### 2.5.1 `pinPage`：查找 → 命中/未命中 → 替换 → 读盘

**中文**

`pinPage` 是整个模块的心脏。它的主干（已删去边界检查和统计细节）：

```c
RC pinPage(BM_BufferPool *const bm, BM_PageHandle *const page,
           const PageNumber pageNum) {
    HashTable *hTable = bm->mgmtData->buffTable;
    int buffId = searchHashTable(hTable, pageNum);   // ① 查哈希表

    if (buffId < 0) {                                // ② 未命中
        node = getFreeNode(bm->mgmtData->freeBuffList);
        if (node == NULL) {                          // 没空槽 → 替换
            node = getListHead(bm->mgmtData->strategyData);
            while (node) {
                buffId = node->buff_id;
                if (buffPoolHeaders[buffId].pinned || fixCount[buffId] > 0)
                    node = node->next;               // 跳过被钉住的
                else { shiftToEnd(strategyData, node); break; }
            }
            if (buffPoolHeaders[buffId].dirtyPage)   // 脏页先写回
                forcePage(bm, pHand);
        } else {                                     // 有空槽：直接用
            appendByNode(strategyData, node);
            buffId = node->buff_id;
        }

        ensureCapacity(pageNum + 1, fHandle);        // ③ 按需扩容
        readBlock(pageNum, fHandle,                  // ④ 读盘
                  &buffPool[buffId * PAGE_SIZE]);
        replaceHashNode(buffTable,                    // ⑤ 更新映射
                        oldPageNum, pageNum, buffId);
        num_reads_disk++;
    } else {                                         // ② 命中
        if (strategy == RS_LRU || strategy == RS_LRU_K)
            shiftNodeById(strategyData, buffId);
        num_buff_hits++;
    }

    buffPoolHeaders[buffId].pinned = TRUE;           // ⑥ 钉住 + fixCount++
    buffPoolHeaders[buffId].pageNumber = pageNum;
    fixCount[buffId]++;
    page->pageNum = pageNum;                         // ⑦ 返回给上层
    page->data = &buffPool[buffId * PAGE_SIZE];
    return RC_OK;
}
```

七个步骤：① 哈希表查 `pageNum` → `buffId`；② 命中走 LRU 移动，未命中继续；③ `ensureCapacity` 让文件长到能装下这一页（第1章的按需扩容）；④ `readBlock` 把磁盘上的页读进槽位；⑤ 在哈希表里把旧 `pageNum` 换成新的；⑥ 钉住这个槽位，`fixCount++`；⑦ 通过 `page->data` 把内存指针暴露给上层——上层之后对这一页的读写都直接走这个指针，零拷贝。

**English**

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

### 2.5.2 `unpinPage` 与 `markDirty`

**中文**

`unpinPage` 是 `pinPage` 的镜像——只是把 `fixCount` 减回去：

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

注意 `pinned` 立刻设为 `FALSE`，但 `fixCount` 只是 **减 1**，不一定归零——同一页可能被多个调用方同时 pin 住，要等最后一个 unpin 完，`fixCount==0` 才允许替换。这是经典的**引用计数**模式。

`markDirty` 同样轻量，就是给对应槽位打上脏标记：

```c
RC markDirty(BM_BufferPool *const bm, BM_PageHandle *const page) {
    int buffId = searchHashTable(bm->mgmtData->buffTable, page->pageNum);
    if (buffId < 0) return RC_DIRTY_FAILED;
    bm->mgmtData->buffPoolHeaders[buffId].dirtyPage = TRUE;
    return RC_OK;
}
```

为什么不直接写回？磁盘写一次要毫秒级，调用方可能改一页改很多次，每次都写回会被 I/O 拖死。**延后写回（deferred write）**是缓冲池的核心优化之一。

**English**

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

### 2.5.3 `forceFlushPool`：脏页写回

**中文**

`forceFlushPool` 把所有脏页写回磁盘。`shutdownBufferPool` 关池前会先调它，确保数据不丢：

```c
RC forceFlushPool(BM_BufferPool *const bm) {
    for (i = 0; i < bm->numPages; ++i) {
        if (bm->mgmtData->buffPoolHeaders[i].dirtyPage) {  // 扫描所有槽位
            pHandle->pageNum = bm->mgmtData->buffPoolHeaders[i].pageNumber;
            forcePage(bm, pHandle);                       // 逐页写回
        }
    }
    return RC_OK;
}
```

`forcePage` 干一件具体的事：把 `buff_id` 槽位的数据 `memcpy` 出来，调第1章的 `writeBlock` 写到磁盘对应页号，清掉 `dirtyPage` 标记（仅在 `fixCount==0` 时），递增 `num_writes_disk` 统计。注意它只在 `fixCount==0` 时清脏标记——如果还有人钉着这一页，写回了也不能算「干净」，因为对方可能马上又改它。

**English**

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

## 2.6 完整 API 一览

| 函数 | 作用 | 一句话 |
|------|------|--------|
| `initBufferPool(bm, name, n, strat, data)` | 初始化 | 开 page file、分配 n 个槽位、建哈希表和策略链表 |
| `shutdownBufferPool(bm)` | 关闭 | 先 flush 所有脏页、检查无 pin、释放所有内存 |
| `forceFlushPool(bm)` | 写回 | 把所有脏页一次性写回磁盘 |
| `markDirty(bm, page)` | 标脏 | 给某页打 dirty 标记 |
| `unpinPage(bm, page)` | 解钉 | `fixCount--`，允许被替换 |
| `forcePage(bm, page)` | 写单页 | 把一页写回磁盘 |
| `pinPage(bm, page, pageNum)` | 钉页 | 命中/未命中/替换/读盘 + `fixCount++` |
| `getFrameContents(bm)` | 元信息 | 各槽位存的 pageNum 数组 |
| `getDirtyFlags(bm)` | 元信息 | 各槽位 dirty 标记数组 |
| `getFixCounts(bm)` | 元信息 | 各槽位 fixCount 数组 |
| `getNumReadIO(bm)` | 统计 | 累计读盘次数 |
| `getNumWriteIO(bm)` | 统计 | 累计写盘次数 |

---

## 2.7 编译与验证

```bash
# 编译整个项目
make all

# 当前没有独立的缓冲池测试目标；记录管理回归测试会间接覆盖缓冲池
./build/test_assign3_1
```

该回归测试通过记录管理器间接覆盖建页文件、`pinPage`、`markDirty`、`unpinPage` 和刷盘路径；它不是缓冲池替换策略的独立单元测试。

如果想自己手测：

```c
BM_BufferPool bm;
BM_PageHandle page;
initBufferPool(&bm, "test.bin", 5, RS_LRU, NULL);

pinPage(&bm, &page, 0);    // miss → 读盘
pinPage(&bm, &page, 1);    // miss → 读盘
pinPage(&bm, &page, 0);    // hit → num_buff_hits++（但不暴露 getter）
markDirty(&bm, &page);
unpinPage(&bm, &page);

printf("reads = %d\n", getNumReadIO(&bm));     // 2
printf("writes = %d\n", getNumWriteIO(&bm));    // 0（还没 flush）

shutdownBufferPool(&bm);    // 关池时自动 flush 脏页
```

---

## 2.8 思考题

1. **`pinned` 是 bool，`fixCount` 是 int——它们俩是不是冗余？能不能只用 `fixCount>0` 来判断一页能不能被换出去？** 提示：看看 `unpinPage` 里 `pinned=FALSE` 和 `fixCount--` 的顺序，以及多个调用方同时 pin 同一页时的语义。

2. **FIFO 策略下，一个页被频繁 pin/unpin 但 `fixCount` 一直 > 0（始终被钉住），它在 `strategyData` 链表里的位置会变化吗？这会带来什么问题？** 提示：FIFO 不在命中时移动节点；考虑「页面驻留」与 FIFO 队列顺序的脱节。

3. **`shutdownBufferPool` 在 flush 完脏页后还要检查「有没有页还被钉住」才能继续关池。为什么不直接强制 unpin 所有页然后关掉？** 提示：谁拥有 `page->data` 指针？强制释放会让上层拿到什么？

---

> **下一章**：[第3章 · 记录管理器](03-record-manager.md)
