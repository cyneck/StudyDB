# Chapter 2 · Buffer Manager

> Source: `src/buffer_mgr.c`, `src/buffer_mgr.h`

## 2.1 Purpose

The buffer manager keeps a fixed number of page copies in memory. A caller pins a page before using it, marks it dirty after modification, and unpins it when finished. Only frames with `fixCount == 0` may be evicted or flushed by `forceFlushPool`. A double unpin is rejected.

## 2.2 Actual implementation

The teaching implementation uses a doubly linked list of private `BufferFrame` objects. Each frame owns one page copy plus a dirty flag, pin count, FIFO entry timestamp, and LRU access timestamp.

Page lookup is a linear O(pool size) walk. There is no hash table or contiguous page array in the current source. Those are possible future optimizations, not current features. Private structures live in `buffer_mgr.c`; the header exposes only public handles and APIs.

## 2.3 Replacement

- FIFO selects the unpinned frame with the smallest entry timestamp.
- LRU selects the unpinned frame with the smallest access timestamp.
- CLOCK, LFU, and LRU-K remain extension points in the enum. Selecting them returns `RC_BM_INVALID_STRATEGY`.

## 2.4 Pin flow

On a hit, `pinPage` increments `fixCount` and refreshes the LRU timestamp. On a miss, it uses an empty frame or an unpinned victim, writes a dirty victim, calls `ensureCapacity(pageNum + 1)`, reads the requested page, and sets its pin count to one.

Pinning page 10 therefore grows a one-page file to 11 pages rather than appending only one page.

## 2.5 Flush and shutdown

- `forcePage` writes one named page.
- `forceFlushPool` writes dirty, unpinned frames.
- `shutdownBufferPool` refuses to close while pins remain, then flushes, closes the file, and frees memory.

`fflush` is not a transactional durability guarantee. This project has no WAL, transactions, or crash recovery.

## 2.6 Verify

```bash
make
./build/test_storage_buffer
```

The suite covers out-of-range reads, sparse growth, dirty writeback, and double unpin.
