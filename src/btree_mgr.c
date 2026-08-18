/**
 * @file btree_mgr.c
 * @authors Xingli Li
 * @date 2023-07-29
 * @brief B+ Tree index manager implementation.
 *
 * Design summary
 * --------------
 * - Each B+ tree lives in its own page file (see btree_mgr.h for layout).
 * - All page I/O goes through the buffer manager (one buffer pool per tree).
 * - Insertion implements full top-down leaf location + bottom-up split with
 *   key promotion to the parent; root splits create a new root.
 * - Deletion removes the key from its leaf but does NOT rebalance the tree
 *   (no borrowing / merging). This is the common CS525 simplification: the
 *   tree stays correct, just possibly under-full. Scan and find still work.
 * - Range scan walks the leaf chain via the `next` pointer.
 *
 * Memory model
 * ------------
 *   BT_MgmtData  -> held by BTreeHandle->mgmtData
 *     .pool        BM_BufferPool*  (one pool per open tree)
 *     .page        BM_PageHandle*  (scratch page handle, reused)
 *     .keyType     cached from page 0
 *     .n           max keys per node (cached)
 *     .rootPage    cached root page number
 *     .numNodes    cached node count
 *     .numEntries  cached entry count
 *
 *   BT_ScanState  -> held by BT_ScanHandle->mgmtData
 *     .currentPage leaf page currently being scanned
 *     .currentSlot index of next key to emit in that leaf
 */
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

#include "btree_mgr.h"
#include "expr.h"
#include "storage_mgr.h"
#include "buffer_mgr.h"

/* ================================================================== */
/*  Constants & layout helpers                                       */
/* ================================================================== */

/** Fixed length used for DT_STRING keys inside the index. */
#define BT_STR_LEN 16

/** Header size inside every node page: isLeaf(4) + numKeys(4) + next(4). */
#define NODE_HDR_SIZE 12

/** Uniform child-slot size (RID = 8 bytes; internal nodes use first 4). */
#define CHILD_SLOT_SIZE 8

/** Bytes needed to store one key of the given type. */
static int
keySizeOf(DataType kt)
{
    switch (kt) {
        case DT_INT:    return (int) sizeof(int);
        case DT_FLOAT:  return (int) sizeof(float);
        case DT_BOOL:   return (int) sizeof(char);
        case DT_STRING: return BT_STR_LEN;
        default:        return (int) sizeof(int);
    }
}

/**
 * Compute the maximum number of keys per node for a given key type.
 * Constraint:  NODE_HDR_SIZE + n*keySize + (n+1)*CHILD_SLOT_SIZE <= PAGE_SIZE
 */
static int
defaultN(DataType kt)
{
    int ks = keySizeOf(kt);
    int n  = (PAGE_SIZE - NODE_HDR_SIZE - CHILD_SLOT_SIZE) / (ks + CHILD_SLOT_SIZE);
    return (n < 4) ? 4 : n;          /* keep at least 4 for sane splits */
}

/* Offset helpers for a node page whose `n` is known. */
#define OFF_ISLEAF(nd)          (0)
#define OFF_NUMKEYS(nd)         (4)
#define OFF_NEXT(nd)            (8)
#define OFF_KEYS(nd, n)         (NODE_HDR_SIZE)
#define OFF_KEY(nd, n, i)       (NODE_HDR_SIZE + (i) * keySizeOf((nd)->keyType))
#define OFF_CHILDREN(nd, n)     (NODE_HDR_SIZE + (n) * keySizeOf((nd)->keyType))
#define OFF_CHILD(nd, n, i)     (OFF_CHILDREN(nd, n) + (i) * CHILD_SLOT_SIZE)

/* ================================================================== */
/*  Internal management structures                                    */
/* ================================================================== */

typedef struct BT_MgmtData {
    BM_BufferPool  *pool;
    BM_PageHandle  *page;
    DataType        keyType;
    int             n;
    int             rootPage;
    int             numNodes;
    int             numEntries;
} BT_MgmtData;

typedef struct BT_ScanState {
    int currentPage;
    int currentSlot;
} BT_ScanState;

/* ================================================================== */
/*  Meta page (page 0) persistence                                   */
/* ================================================================== */

/** Write the cached metadata back to page 0 of the index file. */
static RC
writeMeta(BT_MgmtData *md)
{
    RC rc = pinPage(md->pool, md->page, 0);
    if (rc != RC_OK) return rc;

    char *d = md->page->data;
    int off = 0;
    memcpy(d + off, &md->keyType,    sizeof(int)); off += sizeof(int);
    memcpy(d + off, &md->n,          sizeof(int)); off += sizeof(int);
    memcpy(d + off, &md->rootPage,   sizeof(int)); off += sizeof(int);
    memcpy(d + off, &md->numNodes,   sizeof(int)); off += sizeof(int);
    memcpy(d + off, &md->numEntries, sizeof(int)); off += sizeof(int);

    markDirty(md->pool, md->page);
    unpinPage(md->pool, md->page);
    return RC_OK;
}

/** Read metadata from page 0 into the cache. */
static RC
readMeta(BT_MgmtData *md)
{
    RC rc = pinPage(md->pool, md->page, 0);
    if (rc != RC_OK) return rc;

    char *d = md->page->data;
    int off = 0;
    memcpy(&md->keyType,    d + off, sizeof(int)); off += sizeof(int);
    memcpy(&md->n,          d + off, sizeof(int)); off += sizeof(int);
    memcpy(&md->rootPage,   d + off, sizeof(int)); off += sizeof(int);
    memcpy(&md->numNodes,   d + off, sizeof(int)); off += sizeof(int);
    memcpy(&md->numEntries, d + off, sizeof(int)); off += sizeof(int);

    unpinPage(md->pool, md->page);
    return RC_OK;
}

/* ================================================================== */
/*  Key comparison                                                   */
/* ================================================================== */

/** Returns -1, 0, +1 like strcmp. Both values must share the same dt. */
static int
cmpKeys(const Value *a, const Value *b)
{
    switch (a->dt) {
        case DT_INT:
            if (a->v.intV   < b->v.intV)   return -1;
            if (a->v.intV   > b->v.intV)   return  1;
            return 0;
        case DT_FLOAT:
            if (a->v.floatV < b->v.floatV) return -1;
            if (a->v.floatV > b->v.floatV) return  1;
            return 0;
        case DT_BOOL:
            if (a->v.boolV  < b->v.boolV)  return -1;
            if (a->v.boolV  > b->v.boolV)  return  1;
            return 0;
        case DT_STRING:
            return strncmp(a->v.stringV, b->v.stringV, BT_STR_LEN);
        default:
            return 0;
    }
}

/* ================================================================== */
/*  Node field accessors (operate on a pinned page's `data` buffer)  */
/* ================================================================== */

static int nodeGetIsLeaf(BT_MgmtData *md, char *data) {
    int v; memcpy(&v, data + OFF_ISLEAF(md), sizeof(int)); return v;
}
static void nodeSetIsLeaf(BT_MgmtData *md, char *data, int v) {
    memcpy(data + OFF_ISLEAF(md), &v, sizeof(int));
}
static int nodeGetNumKeys(BT_MgmtData *md, char *data) {
    int v; memcpy(&v, data + OFF_NUMKEYS(md), sizeof(int)); return v;
}
static void nodeSetNumKeys(BT_MgmtData *md, char *data, int v) {
    memcpy(data + OFF_NUMKEYS(md), &v, sizeof(int));
}
static int nodeGetNext(BT_MgmtData *md, char *data) {
    int v; memcpy(&v, data + OFF_NEXT(md), sizeof(int)); return v;
}
static void nodeSetNext(BT_MgmtData *md, char *data, int v) {
    memcpy(data + OFF_NEXT(md), &v, sizeof(int));
}

/** Read keys[i] from a pinned node page into a freshly allocated Value. */
static Value *
nodeGetKey(BT_MgmtData *md, char *data, int i)
{
    Value *v = (Value *) malloc(sizeof(Value));
    v->dt = md->keyType;
    char *src = data + OFF_KEY(md, md->n, i);
    switch (md->keyType) {
        case DT_INT:    memcpy(&v->v.intV,    src, sizeof(int));   break;
        case DT_FLOAT:  memcpy(&v->v.floatV,  src, sizeof(float)); break;
        case DT_BOOL:   memcpy(&v->v.boolV,   src, sizeof(char));  break;
        case DT_STRING:
            v->v.stringV = (char *) malloc(BT_STR_LEN + 1);
            memcpy(v->v.stringV, src, BT_STR_LEN);
            v->v.stringV[BT_STR_LEN] = '\0';
            break;
    }
    return v;
}

/** Write `v` into keys[i] of a pinned node page. */
static void
nodeSetKey(BT_MgmtData *md, char *data, int i, const Value *v)
{
    char *dst = data + OFF_KEY(md, md->n, i);
    switch (md->keyType) {
        case DT_INT:    memcpy(dst, &v->v.intV,   sizeof(int));   break;
        case DT_FLOAT:  memcpy(dst, &v->v.floatV, sizeof(float)); break;
        case DT_BOOL:   memcpy(dst, &v->v.boolV,  sizeof(char));  break;
        case DT_STRING: memcpy(dst, v->v.stringV, BT_STR_LEN);    break;
    }
}

/** Read child slot i. For leaves returns an RID; for internal returns page in RID.page. */
static RID
nodeGetChild(BT_MgmtData *md, char *data, int i)
{
    RID r;
    memcpy(&r, data + OFF_CHILD(md, md->n, i), sizeof(RID));
    return r;
}

/** Write child slot i. */
static void
nodeSetChild(BT_MgmtData *md, char *data, int i, RID r)
{
    memcpy(data + OFF_CHILD(md, md->n, i), &r, sizeof(RID));
}

/* ================================================================== */
/*  Index manager lifecycle                                          */
/* ================================================================== */

RC
initIndexManager(void *mgmtData)
{
    /* Nothing global to set up; each tree owns its own buffer pool. */
    (void) mgmtData;
    initStorageManager();
    return RC_OK;
}

RC
shutdownIndexManager()
{
    return RC_OK;
}

/* ------------------------------------------------------------------ */
/*  Tree file lifecycle                                              */
/* ------------------------------------------------------------------ */

RC
createBTree(char *idxId, DataType keyType, int n)
{
    if (idxId == NULL)
        return RC_NULL_POINTER;

    /* 1. create the underlying page file */
    CHECKEX(createPageFile(idxId));

    /* 2. build a temporary mgmt struct to write the meta page */
    BT_MgmtData *md = (BT_MgmtData *) malloc(sizeof(BT_MgmtData));
    md->pool = MAKE_POOL();
    md->page = MAKE_PAGE_HANDLE();
    md->keyType = keyType;
    md->n = (n > 0) ? n : defaultN(keyType);
    md->rootPage = 0;            /* empty tree */
    md->numNodes = 0;
    md->numEntries = 0;

    RC rc = initBufferPool(md->pool, idxId, 10, RS_FIFO, NULL);
    if (rc != RC_OK) { free(md->pool); free(md->page); free(md); return rc; }

    rc = writeMeta(md);
    if (rc != RC_OK) {
        shutdownBufferPool(md->pool);
        free(md->pool); free(md->page); free(md);
        return rc;
    }

    shutdownBufferPool(md->pool);
    free(md->pool);
    free(md->page);
    free(md);
    return RC_OK;
}

RC
openBTree(BTreeHandle **tree, char *idxId)
{
    if (tree == NULL || idxId == NULL)
        return RC_NULL_POINTER;

    BTreeHandle *h = (BTreeHandle *) malloc(sizeof(BTreeHandle));
    BT_MgmtData *md = (BT_MgmtData *) malloc(sizeof(BT_MgmtData));
    md->pool = MAKE_POOL();
    md->page = MAKE_PAGE_HANDLE();

    RC rc = initBufferPool(md->pool, idxId, 10, RS_FIFO, NULL);
    if (rc != RC_OK) {
        free(md->pool); free(md->page); free(md); free(h);
        return rc;
    }

    rc = readMeta(md);
    if (rc != RC_OK) {
        shutdownBufferPool(md->pool);
        free(md->pool); free(md->page); free(md); free(h);
        return rc;
    }

    h->keyType = md->keyType;
    h->idxId = (char *) malloc(strlen(idxId) + 1);
    strcpy(h->idxId, idxId);
    h->mgmtData = md;

    *tree = h;
    return RC_OK;
}

RC
closeBTree(BTreeHandle **tree)
{
    if (tree == NULL || *tree == NULL)
        return RC_NULL_POINTER;

    BTreeHandle *h = *tree;
    BT_MgmtData *md = (BT_MgmtData *) h->mgmtData;

    /* persist any in-memory meta changes (numEntries, rootPage, ...) */
    writeMeta(md);

    shutdownBufferPool(md->pool);
    free(md->pool);
    free(md->page);
    free(md);
    free(h->idxId);
    free(h);
    *tree = NULL;
    return RC_OK;
}

RC
deleteBTree(char *idxId)
{
    if (idxId == NULL)
        return RC_NULL_POINTER;
    return destroyPageFile(idxId);
}

/* ------------------------------------------------------------------ */
/*  Statistics                                                       */
/* ------------------------------------------------------------------ */

RC getNumNodes(BTreeHandle *tree, int *result) {
    if (!tree || !result) return RC_NULL_POINTER;
    *result = ((BT_MgmtData *) tree->mgmtData)->numNodes;
    return RC_OK;
}

RC getNumEntries(BTreeHandle *tree, int *result) {
    if (!tree || !result) return RC_NULL_POINTER;
    *result = ((BT_MgmtData *) tree->mgmtData)->numEntries;
    return RC_OK;
}

RC getKeyType(BTreeHandle *tree, DataType *result) {
    if (!tree || !result) return RC_NULL_POINTER;
    *result = tree->keyType;
    return RC_OK;
}

/* ================================================================== */
/*  Insertion                                                        */
/* ================================================================== */

/**
 * Allocate a brand-new node page, return its page number.
 * The page is initialised with isLeaf, numKeys=0, next=-1 and left dirty
 * so the caller can fill in the body. Increments md->numNodes.
 */
static int
allocNode(BT_MgmtData *md, int isLeaf)
{
    /* page 0 is meta; first node page goes at index (numNodes + 1)
     * because createPageFile made exactly 1 data page (page 0) and the
     * buffer manager's ensureCapacity grows the file as we pin higher. */
    int newPage = md->numNodes + 1;

    RC rc = pinPage(md->pool, md->page, newPage);
    if (rc != RC_OK) return -1;

    char *d = md->page->data;
    memset(d, 0, PAGE_SIZE);
    nodeSetIsLeaf(md, d, isLeaf ? 1 : 0);
    nodeSetNumKeys(md, d, 0);
    nodeSetNext(md, d, -1);

    markDirty(md->pool, md->page);
    unpinPage(md->pool, md->page);

    md->numNodes++;
    return newPage;
}

/**
 * Insert (key, rid) into a leaf `leafPage` that is known to have room.
 * Keeps keys sorted. Returns RC_OK.
 */
static RC
leafInsertSorted(BT_MgmtData *md, int leafPage, Value *key, RID rid)
{
    RC rc = pinPage(md->pool, md->page, leafPage);
    if (rc != RC_OK) return rc;
    char *d = md->page->data;

    int k = nodeGetNumKeys(md, d);
    int pos = k;
    /* find position via linear scan (k is small, < n) */
    for (int i = 0; i < k; i++) {
        Value *ki = nodeGetKey(md, d, i);
        if (cmpKeys(key, ki) < 0) { pos = i; freeVal(ki); break; }
        freeVal(ki);
    }
    /* shift right to make room at `pos` */
    for (int i = k; i > pos; i--) {
        Value *v = nodeGetKey(md, d, i - 1);
        nodeSetKey(md, d, i, v);
        freeVal(v);
        RID r = nodeGetChild(md, d, i - 1);
        nodeSetChild(md, d, i, r);
    }
    nodeSetKey(md, d, pos, key);
    nodeSetChild(md, d, pos, rid);
    nodeSetNumKeys(md, d, k + 1);

    markDirty(md->pool, md->page);
    unpinPage(md->pool, md->page);
    return RC_OK;
}

/**
 * Split a full node `pageNo` (n keys) into pageNo + newRight.
 * `key` and `rid` are the entry that caused the overflow; they will be
 * inserted into the appropriate half. On return:
 *   - *upKey   receives the key promoted to the parent
 *   - *rightPg receives the new right-sibling page number
 *
 * Leaf vs internal differ in child accounting:
 *   leaf     : n keys  + n rids      + 1 new (key,rid)   -> n+1 keys, n+1 rids
 *   internal : n keys  + (n+1) pages + 1 new (key,page)  -> n+1 keys, n+2 pages
 *
 * Returns RC_OK or an error.
 */
static RC
splitNode(BT_MgmtData *md, int pageNo, Value *key, RID rid,
          Value **upKey, int *rightPg)
{
    RC rc = pinPage(md->pool, md->page, pageNo);
    if (rc != RC_OK) return rc;
    char *leftData = md->page->data;

    int isLeaf = nodeGetIsLeaf(md, leftData);
    int n = md->n;

    /* Capture the leaf's current `next` pointer BEFORE we unpin, so the
     * new right sibling can inherit it and keep the leaf chain intact. */
    int oldNext = nodeGetNext(md, leftData);

    /* ---- Build merged overflow buffers ---------------------------- */
    int totalKeys = n + 1;
    Value **allKeys = (Value **) malloc(sizeof(Value *) * totalKeys);
    /* leaves need totalKeys rids; internals need totalKeys+1 pages */
    int totalRids = isLeaf ? totalKeys : totalKeys + 1;
    RID    *allRids = (RID *)    malloc(sizeof(RID)    * totalRids);

    /* find insertion position in the existing sorted key list */
    int pos = n;
    for (int i = 0; i < n; i++) {
        Value *ki = nodeGetKey(md, leftData, i);
        if (cmpKeys(key, ki) < 0) { pos = i; freeVal(ki); break; }
        freeVal(ki);
    }

    /* fill merged keys/rids */
    int idx = 0;
    for (int i = 0; i < totalKeys; i++) {
        if (i == pos) {
            /* the new entry */
            allKeys[i] = (Value *) malloc(sizeof(Value));
            allKeys[i]->dt = key->dt;
            if (key->dt == DT_STRING) {
                allKeys[i]->v.stringV = (char *) malloc(BT_STR_LEN + 1);
                strncpy(allKeys[i]->v.stringV, key->v.stringV, BT_STR_LEN);
                allKeys[i]->v.stringV[BT_STR_LEN] = '\0';
            } else {
                memcpy(&allKeys[i]->v, &key->v, sizeof(allKeys[i]->v));
            }
        } else {
            allKeys[i] = nodeGetKey(md, leftData, idx);
            idx++;
        }
    }

    if (isLeaf) {
        /* leaves: rids[i] aligns with keys[i] */
        idx = 0;
        for (int i = 0; i < totalKeys; i++) {
            if (i == pos) {
                allRids[i] = rid;
            } else {
                allRids[i] = nodeGetChild(md, leftData, idx);
                idx++;
            }
        }
    } else {
        /* internal: children[i] is the slot before key[i]; children[i+1]
         * is the slot after key[i]. The new (key, page) inserts a new
         * child at position pos+1. */
        for (int i = 0; i <= n; i++) {
            allRids[i] = nodeGetChild(md, leftData, i);
        }
        /* shift children[pos+1..n] right by one to make room at pos+1 */
        for (int i = n; i >= pos + 1; i--) {
            allRids[i + 1] = allRids[i];
        }
        allRids[pos + 1] = rid;     /* rid.page holds the new child page */
    }

    /* we no longer need the pinned left page right now */
    unpinPage(md->pool, md->page);

    /* ---- Allocate the right sibling ------------------------------- */
    int rightPage = allocNode(md, isLeaf);
    *rightPg = rightPage;

    int mid = totalKeys / 2;

    /* ---- Rewrite the left node with the smaller half -------------- */
    rc = pinPage(md->pool, md->page, pageNo);
    if (rc != RC_OK) goto cleanup;
    {
        char *ld = md->page->data;
        int leftCount = mid;     /* keys[0..mid-1] stay on the left */
        for (int i = 0; i < leftCount; i++) {
            nodeSetKey(md, ld, i, allKeys[i]);
        }
        if (isLeaf) {
            for (int i = 0; i < leftCount; i++) {
                nodeSetChild(md, ld, i, allRids[i]);
            }
            nodeSetNext(md, ld, rightPage);
        } else {
            /* left children[0..mid] = allRids[0..mid] */
            for (int i = 0; i <= leftCount; i++) {
                nodeSetChild(md, ld, i, allRids[i]);
            }
            nodeSetNext(md, ld, -1);
        }
        nodeSetNumKeys(md, ld, leftCount);
        markDirty(md->pool, md->page);
        unpinPage(md->pool, md->page);
    }

    /* ---- Fill the right node with the larger half ----------------- */
    rc = pinPage(md->pool, md->page, rightPage);
    if (rc != RC_OK) goto cleanup;
    {
        char *rd = md->page->data;
        if (isLeaf) {
            /* right gets keys[mid..n], rids[mid..n]; promoted = keys[mid] (copy up) */
            int rCount = totalKeys - mid;
            for (int i = 0; i < rCount; i++) {
                nodeSetKey(md, rd, i, allKeys[mid + i]);
                nodeSetChild(md, rd, i, allRids[mid + i]);
            }
            nodeSetNumKeys(md, rd, rCount);
            /* right inherits left's old `next` so the leaf chain stays
             * linked: left -> right -> (old next). For the rightmost leaf
             * oldNext is -1, which is exactly what we want. */
            nodeSetNext(md, rd, oldNext);

            *upKey = (Value *) malloc(sizeof(Value));
            (*upKey)->dt = allKeys[mid]->dt;
            if ((*upKey)->dt == DT_STRING) {
                (*upKey)->v.stringV = (char *) malloc(BT_STR_LEN + 1);
                strncpy((*upKey)->v.stringV, allKeys[mid]->v.stringV, BT_STR_LEN);
                (*upKey)->v.stringV[BT_STR_LEN] = '\0';
            } else {
                memcpy(&(*upKey)->v, &allKeys[mid]->v, sizeof((*upKey)->v));
            }
        } else {
            /* internal: promote keys[mid] (push up); right gets keys[mid+1..n]
             * and children[mid+1..n+1] */
            int rCount = totalKeys - mid - 1;
            for (int i = 0; i < rCount; i++) {
                nodeSetKey(md, rd, i, allKeys[mid + 1 + i]);
            }
            for (int i = 0; i <= rCount; i++) {
                nodeSetChild(md, rd, i, allRids[mid + 1 + i]);
            }
            nodeSetNumKeys(md, rd, rCount);
            nodeSetNext(md, rd, -1);

            *upKey = (Value *) malloc(sizeof(Value));
            (*upKey)->dt = allKeys[mid]->dt;
            if ((*upKey)->dt == DT_STRING) {
                (*upKey)->v.stringV = (char *) malloc(BT_STR_LEN + 1);
                strncpy((*upKey)->v.stringV, allKeys[mid]->v.stringV, BT_STR_LEN);
                (*upKey)->v.stringV[BT_STR_LEN] = '\0';
            } else {
                memcpy(&(*upKey)->v, &allKeys[mid]->v, sizeof((*upKey)->v));
            }
        }
        markDirty(md->pool, md->page);
        unpinPage(md->pool, md->page);
    }

cleanup:
    for (int i = 0; i < totalKeys; i++) freeVal(allKeys[i]);
    free(allKeys);
    free(allRids);
    return rc;
}

/**
 * Recursive insert into the subtree rooted at `pageNo`.
 * `key`/`rid` are the new entry. On split, *splitKey and *splitRight are
 * set (caller must propagate them up). *didSplit is set to 1 if a split
 * happened, 0 otherwise.
 */
static RC
insertRecursive(BT_MgmtData *md, int pageNo, Value *key, RID rid,
                int *didSplit, Value **splitKey, int *splitRight)
{
    *didSplit = 0;
    RC rc = pinPage(md->pool, md->page, pageNo);
    if (rc != RC_OK) return rc;

    /* Snapshot the fields we need before any unpin. */
    int isLeaf  = nodeGetIsLeaf(md, md->page->data);
    int numKeys = nodeGetNumKeys(md, md->page->data);

    if (isLeaf) {
        unpinPage(md->pool, md->page);

        if (numKeys < md->n) {
            return leafInsertSorted(md, pageNo, key, rid);
        }
        /* full -> split this leaf */
        rc = splitNode(md, pageNo, key, rid, splitKey, splitRight);
        if (rc == RC_OK) *didSplit = 1;
        return rc;
    }

    /* Internal: pick the child to descend into.
     * Child i is the subtree for keys in (keys[i-1], keys[i]] ;
     * conventionally children[0] holds keys <= keys[0], children[i] holds
     * keys in (keys[i-1], keys[i]] for i in [1, numKeys-1], and
     * children[numKeys] holds keys > keys[numKeys-1].
     */
    int childIdx = numKeys;     /* default: last child */
    for (int i = 0; i < numKeys; i++) {
        Value *ki = nodeGetKey(md, md->page->data, i);
        if (cmpKeys(key, ki) < 0) { childIdx = i; freeVal(ki); break; }
        freeVal(ki);
    }
    RID childRid = nodeGetChild(md, md->page->data, childIdx);
    int childPage = childRid.page;
    unpinPage(md->pool, md->page);

    Value *subSplitKey = NULL;
    int    subSplitRight = -1;
    int    subDidSplit = 0;
    rc = insertRecursive(md, childPage, key, rid,
                         &subDidSplit, &subSplitKey, &subSplitRight);
    if (rc != RC_OK) return rc;
    if (!subDidSplit) return RC_OK;

    /* Child split -> we may need to insert (subSplitKey, subSplitRight)
     * into this internal node. First check if we have room. */
    rc = pinPage(md->pool, md->page, pageNo);
    if (rc != RC_OK) { freeVal(subSplitKey); return rc; }
    numKeys = nodeGetNumKeys(md, md->page->data);

    if (numKeys < md->n) {
        /* room: insert (subSplitKey, subSplitRight) at the right place.
         * Internal node children have numKeys+1 slots; inserting a new
         * key at position `pos` requires a new child at pos+1. */
        int pos = numKeys;
        for (int i = 0; i < numKeys; i++) {
            Value *ki = nodeGetKey(md, md->page->data, i);
            if (cmpKeys(subSplitKey, ki) < 0) { pos = i; freeVal(ki); break; }
            freeVal(ki);
        }
        /* shift children[numKeys+1 .. pos+1] = children[numKeys .. pos]
         * (iterate right-to-left to avoid overwriting) */
        for (int i = numKeys; i >= pos; i--) {
            RID r = nodeGetChild(md, md->page->data, i);
            nodeSetChild(md, md->page->data, i + 1, r);
        }
        /* shift keys[numKeys .. pos+1] = keys[numKeys-1 .. pos] */
        for (int i = numKeys - 1; i >= pos; i--) {
            Value *v = nodeGetKey(md, md->page->data, i);
            nodeSetKey(md, md->page->data, i + 1, v);
            freeVal(v);
        }
        nodeSetKey(md, md->page->data, pos, subSplitKey);
        RID newR; newR.page = subSplitRight; newR.slot = 0;
        nodeSetChild(md, md->page->data, pos + 1, newR);
        nodeSetNumKeys(md, md->page->data, numKeys + 1);

        markDirty(md->pool, md->page);
        unpinPage(md->pool, md->page);
        freeVal(subSplitKey);
        return RC_OK;
    }

    /* No room here either -> split this internal node too.
     * We treat (subSplitKey, subSplitRight) as the new entry. */
    unpinPage(md->pool, md->page);

    /* synthesize an RID carrying subSplitRight as .page so splitNode can
     * treat the new entry uniformly with existing child slots. */
    RID newChildRid; newChildRid.page = subSplitRight; newChildRid.slot = 0;
    rc = splitNode(md, pageNo, subSplitKey, newChildRid, splitKey, splitRight);
    freeVal(subSplitKey);
    if (rc == RC_OK) *didSplit = 1;
    return rc;
}

RC
insertKey(BTreeHandle *tree, Value *key, RID rid)
{
    if (!tree || !key) return RC_NULL_POINTER;
    BT_MgmtData *md = (BT_MgmtData *) tree->mgmtData;

    /* duplicate check */
    RID existing;
    if (findKey(tree, key, &existing) == RC_OK)
        return RC_IM_KEY_ALREADY_EXISTS;

    if (md->rootPage == 0) {
        /* empty tree: create the first leaf as root */
        int root = allocNode(md, 1 /*leaf*/);
        md->rootPage = root;
        md->numEntries++;
        writeMeta(md);
        return leafInsertSorted(md, root, key, rid);
    }

    int didSplit = 0;
    Value *splitKey = NULL;
    int    splitRight = -1;
    RC rc = insertRecursive(md, md->rootPage, key, rid,
                            &didSplit, &splitKey, &splitRight);
    if (rc != RC_OK) return rc;

    if (didSplit) {
        /* root split -> create a new root pointing to old root + right */
        int newRoot = allocNode(md, 0 /*internal*/);
        rc = pinPage(md->pool, md->page, newRoot);
        if (rc != RC_OK) { freeVal(splitKey); return rc; }
        char *d = md->page->data;

        nodeSetKey(md, d, 0, splitKey);
        RID l; l.page = md->rootPage; l.slot = 0;
        RID r; r.page = splitRight;   r.slot = 0;
        nodeSetChild(md, d, 0, l);
        nodeSetChild(md, d, 1, r);
        nodeSetNumKeys(md, d, 1);
        nodeSetNext(md, d, -1);

        markDirty(md->pool, md->page);
        unpinPage(md->pool, md->page);

        md->rootPage = newRoot;
        freeVal(splitKey);
    }

    md->numEntries++;
    writeMeta(md);
    return RC_OK;
}

/* ================================================================== */
/*  findKey                                                          */
/* ================================================================== */

RC
findKey(BTreeHandle *tree, Value *key, RID *result)
{
    if (!tree || !key || !result) return RC_NULL_POINTER;
    BT_MgmtData *md = (BT_MgmtData *) tree->mgmtData;

    if (md->rootPage == 0)
        return RC_IM_KEY_NOT_FOUND;

    int page = md->rootPage;
    while (page > 0) {
        RC rc = pinPage(md->pool, md->page, page);
        if (rc != RC_OK) return rc;
        char *d = md->page->data;
        int isLeaf  = nodeGetIsLeaf(md, d);
        int numKeys = nodeGetNumKeys(md, d);

        int i = 0;
        for (; i < numKeys; i++) {
            Value *ki = nodeGetKey(md, d, i);
            int c = cmpKeys(key, ki);
            freeVal(ki);
            if (c < 0) break;
        }

        if (isLeaf) {
            /* linear scan for exact match; stop early once keys exceed
             * the search key (they are sorted). */
            for (int j = 0; j < numKeys; j++) {
                Value *ki = nodeGetKey(md, d, j);
                int c = cmpKeys(key, ki);
                freeVal(ki);
                if (c == 0) {
                    *result = nodeGetChild(md, d, j);
                    unpinPage(md->pool, md->page);
                    return RC_OK;
                }
                if (c < 0) break;
            }
            unpinPage(md->pool, md->page);
            return RC_IM_KEY_NOT_FOUND;
        }

        /* internal: descend */
        RID r = nodeGetChild(md, d, i);
        page = r.page;
        unpinPage(md->pool, md->page);
    }
    return RC_IM_KEY_NOT_FOUND;
}

/* ================================================================== */
/*  deleteKey                                                        */
/* ================================================================== */

RC
deleteKey(BTreeHandle *tree, Value *key)
{
    if (!tree || !key) return RC_NULL_POINTER;
    BT_MgmtData *md = (BT_MgmtData *) tree->mgmtData;

    if (md->rootPage == 0)
        return RC_IM_KEY_NOT_FOUND;

    /* locate the leaf, then delete by shifting. No rebalancing. */
    int page = md->rootPage;
    while (page > 0) {
        RC rc = pinPage(md->pool, md->page, page);
        if (rc != RC_OK) return rc;
        char *d = md->page->data;
        int isLeaf  = nodeGetIsLeaf(md, d);
        int numKeys = nodeGetNumKeys(md, d);

        int i = 0;
        for (; i < numKeys; i++) {
            Value *ki = nodeGetKey(md, d, i);
            int c = cmpKeys(key, ki);
            freeVal(ki);
            if (c < 0) break;
        }

        if (isLeaf) {
            /* linear scan for the key to delete. */
            for (int j = 0; j < numKeys; j++) {
                Value *ki = nodeGetKey(md, d, j);
                int c = cmpKeys(key, ki);
                freeVal(ki);
                if (c == 0) {
                    /* shift left to overwrite the deleted entry */
                    for (int k = j; k < numKeys - 1; k++) {
                        Value *v = nodeGetKey(md, d, k + 1);
                        nodeSetKey(md, d, k, v);
                        freeVal(v);
                        RID r = nodeGetChild(md, d, k + 1);
                        nodeSetChild(md, d, k, r);
                    }
                    nodeSetNumKeys(md, d, numKeys - 1);
                    markDirty(md->pool, md->page);
                    unpinPage(md->pool, md->page);
                    md->numEntries--;
                    writeMeta(md);
                    return RC_OK;
                }
                if (c < 0) break;
            }
            unpinPage(md->pool, md->page);
            return RC_IM_KEY_NOT_FOUND;
        }

        RID r = nodeGetChild(md, d, i);
        page = r.page;
        unpinPage(md->pool, md->page);
    }
    return RC_IM_KEY_NOT_FOUND;
}

/* ================================================================== */
/*  Range scan                                                       */
/* ================================================================== */

RC
openTreeScan(BTreeHandle *tree, BT_ScanHandle **handle)
{
    if (!tree || !handle) return RC_NULL_POINTER;
    BT_MgmtData *md = (BT_MgmtData *) tree->mgmtData;

    if (md->rootPage == 0)
        return RC_IM_NO_MORE_ENTRIES;

    /* descend to the leftmost leaf */
    int page = md->rootPage;
    while (page > 0) {
        RC rc = pinPage(md->pool, md->page, page);
        if (rc != RC_OK) return rc;
        char *d = md->page->data;
        int isLeaf = nodeGetIsLeaf(md, d);
        if (isLeaf) { unpinPage(md->pool, md->page); break; }
        RID r = nodeGetChild(md, d, 0);
        page = r.page;
        unpinPage(md->pool, md->page);
    }

    BT_ScanHandle *h = (BT_ScanHandle *) malloc(sizeof(BT_ScanHandle));
    BT_ScanState *st = (BT_ScanState *) malloc(sizeof(BT_ScanState));
    st->currentPage = page;
    st->currentSlot = 0;
    h->tree = tree;
    h->mgmtData = st;
    *handle = h;
    return RC_OK;
}

RC
nextEntry(BT_ScanHandle *handle, RID *result)
{
    if (!handle || !result) return RC_NULL_POINTER;
    BT_MgmtData *md = (BT_MgmtData *) handle->tree->mgmtData;
    BT_ScanState *st = (BT_ScanState *) handle->mgmtData;

    while (st->currentPage > 0) {
        RC rc = pinPage(md->pool, md->page, st->currentPage);
        if (rc != RC_OK) return rc;
        char *d = md->page->data;
        int numKeys = nodeGetNumKeys(md, d);

        if (st->currentSlot < numKeys) {
            *result = nodeGetChild(md, d, st->currentSlot);
            st->currentSlot++;
            unpinPage(md->pool, md->page);
            return RC_OK;
        }

        /* advance to next leaf */
        int nxt = nodeGetNext(md, d);
        unpinPage(md->pool, md->page);
        st->currentPage = nxt;
        st->currentSlot = 0;
    }
    return RC_IM_NO_MORE_ENTRIES;
}

RC
closeTreeScan(BT_ScanHandle **handle)
{
    if (!handle || !*handle) return RC_NULL_POINTER;
    BT_ScanHandle *h = *handle;
    free(h->mgmtData);
    free(h);
    *handle = NULL;
    return RC_OK;
}
