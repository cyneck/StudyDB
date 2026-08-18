/**
 * @file btree_mgr.h
 * @authors Xingli Li
 * @date 2023-07-29
 * @brief B+ Tree index manager interface.
 *
 * Each B+ tree is stored in its own page file (an "index file"). The first
 * page (page 0) holds the tree metadata; subsequent pages hold tree nodes.
 * All page access goes through the existing buffer manager so that the
 * FIFO/LRU/LRU_K strategies transparently apply to the index too.
 *
 * Page 0 (meta) layout:
 *   [0..3]   keyType   (DataType, int)
 *   [4..7]   n         (max keys per node)
 *   [8..11]  rootPage  (page number of the root, 0 if empty tree)
 *   [12..15] numNodes  (number of node pages in use)
 *   [16..19] numEntries(number of keys stored)
 *
 * Node page layout (header + body):
 *   header (12 bytes):
 *     [0..3]   isLeaf   (0 = internal, 1 = leaf)
 *     [4..7]   numKeys
 *     [8..11]  next     (leaf: next-leaf page no; internal: unused, -1)
 *   body:
 *     keys[0..n-1]              (n * keySize bytes)
 *     children[0..n]            ((n+1) * childSlot bytes)
 *
 *   For leaf nodes   children[i] is an RID (page, slot) -- 8 bytes.
 *   For internal     children[i] is a child page number   -- 4 bytes, but
 *                     stored in an 8-byte slot to keep the layout uniform.
 *
 * Leaf nodes are linked left-to-right via `next` to support range scans.
 */
#ifndef BTREE_MGR_H
#define BTREE_MGR_H

#include "dberror.h"
#include "tables.h"
#include "buffer_mgr.h"

/* ------------------------------------------------------------------ */
/*  Public handle types                                               */
/* ------------------------------------------------------------------ */

/** Handle to an open B+ tree. `mgmtData` holds the buffer pool and meta. */
typedef struct BTreeHandle {
    DataType keyType;
    char *idxId;
    void *mgmtData;
} BTreeHandle;

/** Handle to an in-progress tree scan. */
typedef struct BT_ScanHandle {
    BTreeHandle *tree;
    void *mgmtData;
} BT_ScanHandle;

/* ------------------------------------------------------------------ */
/*  Index manager lifecycle                                          */
/* ------------------------------------------------------------------ */
extern RC initIndexManager(void *mgmtData);
extern RC shutdownIndexManager();

/* ------------------------------------------------------------------ */
/*  Tree file lifecycle                                              */
/* ------------------------------------------------------------------ */

/**
 * @brief Create a new (empty) B+ tree stored in page file `idxId`.
 * @param idxId   page file name to create
 * @param keyType datatype of the indexed key
 * @param n       max keys per node; if n <= 0 a sensible default is computed
 *                from PAGE_SIZE and keyType.
 */
extern RC createBTree(char *idxId, DataType keyType, int n);

extern RC openBTree(BTreeHandle **tree, char *idxId);
extern RC closeBTree(BTreeHandle **tree);
extern RC deleteBTree(char *idxId);

/* ------------------------------------------------------------------ */
/*  Tree statistics                                                  */
/* ------------------------------------------------------------------ */
extern RC getNumNodes(BTreeHandle *tree, int *result);
extern RC getNumEntries(BTreeHandle *tree, int *result);
extern RC getKeyType(BTreeHandle *tree, DataType *result);

/* ------------------------------------------------------------------ */
/*  Key operations                                                   */
/* ------------------------------------------------------------------ */

/** Locate the RID associated with `key`; RC_IM_KEY_NOT_FOUND if absent. */
extern RC findKey(BTreeHandle *tree, Value *key, RID *result);

/** Insert (key, rid). RC_IM_KEY_ALREADY_EXISTS if the key is present. */
extern RC insertKey(BTreeHandle *tree, Value *key, RID rid);

/** Remove `key`. RC_IM_KEY_NOT_FOUND if absent. */
extern RC deleteKey(BTreeHandle *tree, Value *key);

/* ------------------------------------------------------------------ */
/*  Range scan                                                       */
/* ------------------------------------------------------------------ */
extern RC openTreeScan(BTreeHandle *tree, BT_ScanHandle **handle);
extern RC nextEntry(BT_ScanHandle *handle, RID *result);
extern RC closeTreeScan(BT_ScanHandle **handle);

#endif /* BTREE_MGR_H */
