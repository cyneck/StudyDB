/**
 * @file test_btree.c
 * @authors Xingli Li
 * @date 2023-07-29
 * @brief B+ tree index manager unit tests.
 *
 * Exercises: create / open / close / delete, ordered + reverse-ordered
 * insertion (forces multiple splits and at least one root split),
 * point lookup, duplicate-rejection, deletion, and full range scan.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "dberror.h"
#include "tables.h"
#include "expr.h"
#include "btree_mgr.h"
#include "test_helper.h"

static void testCreateOpenClose(void);
static void testInsertAndFind(void);
static void testReverseInsert(void);
static void testScanOrder(void);
static void testDelete(void);
static void testDuplicate(void);

char *testName;

int
main(void)
{
    testName = "";
    initIndexManager(NULL);

    testCreateOpenClose();
    testInsertAndFind();
    testReverseInsert();
    testScanOrder();
    testDelete();
    testDuplicate();

    shutdownIndexManager();
    printf("ALL BTREE TESTS PASSED\n");
    return 0;
}

/* ------------------------------------------------------------------ */
static void
testCreateOpenClose(void)
{
    BTreeHandle *tree = NULL;
    testName = "btree: create / open / close / delete";

    TEST_CHECK(createBTree("test_idx_a", DT_INT, 4));
    TEST_CHECK(openBTree(&tree, "test_idx_a"));

    int nodes, entries;
    DataType kt;
    TEST_CHECK(getNumNodes(tree, &nodes));
    TEST_CHECK(getNumEntries(tree, &entries));
    TEST_CHECK(getKeyType(tree, &kt));
    ASSERT_EQUALS_INT(0, nodes, "empty tree has 0 nodes");
    ASSERT_EQUALS_INT(0, entries, "empty tree has 0 entries");
    ASSERT_EQUALS_INT(DT_INT, (int) kt, "key type preserved");

    TEST_CHECK(closeBTree(&tree));
    TEST_CHECK(deleteBTree("test_idx_a"));
    TEST_DONE();
}

/* ------------------------------------------------------------------ */
static void
testInsertAndFind(void)
{
    BTreeHandle *tree = NULL;
    testName = "btree: insert ascending + find";

    TEST_CHECK(createBTree("test_idx_b", DT_INT, 4));
    TEST_CHECK(openBTree(&tree, "test_idx_b"));

    /* insert 20 keys in ascending order: forces leaf splits and
     * eventually an internal split + new root */
    for (int i = 0; i < 20; i++) {
        Value *k; MAKE_VALUE(k, DT_INT, i * 10);
        RID r = { .page = i, .slot = i };
        TEST_CHECK(insertKey(tree, k, r));
        freeVal(k);
    }

    int entries;
    TEST_CHECK(getNumEntries(tree, &entries));
    ASSERT_EQUALS_INT(20, entries, "20 entries stored");

    /* find each key back */
    for (int i = 0; i < 20; i++) {
        Value *k; MAKE_VALUE(k, DT_INT, i * 10);
        RID r;
        TEST_CHECK(findKey(tree, k, &r));
        ASSERT_EQUALS_INT(i, r.page, "rid.page preserved");
        ASSERT_EQUALS_INT(i, r.slot, "rid.slot preserved");
        freeVal(k);
    }

    /* a missing key */
    Value *missing; MAKE_VALUE(missing, DT_INT, 9999);
    ASSERT_ERROR(findKey(tree, missing, &(RID){0}), "missing key not found");
    freeVal(missing);

    TEST_CHECK(closeBTree(&tree));
    TEST_CHECK(deleteBTree("test_idx_b"));
    TEST_DONE();
}

/* ------------------------------------------------------------------ */
static void
testReverseInsert(void)
{
    BTreeHandle *tree = NULL;
    testName = "btree: insert descending (stress splits)";

    TEST_CHECK(createBTree("test_idx_c", DT_INT, 4));
    TEST_CHECK(openBTree(&tree, "test_idx_c"));

    for (int i = 50; i >= 0; i--) {
        Value *k; MAKE_VALUE(k, DT_INT, i);
        RID r = { .page = i, .slot = 0 };
        TEST_CHECK(insertKey(tree, k, r));
        freeVal(k);
    }

    int entries;
    TEST_CHECK(getNumEntries(tree, &entries));
    ASSERT_EQUALS_INT(51, entries, "51 entries after reverse insert");

    /* sanity: find first and last */
    Value *k1; MAKE_VALUE(k1, DT_INT, 0);
    RID r1; TEST_CHECK(findKey(tree, k1, &r1));
    ASSERT_EQUALS_INT(0, r1.page, "key 0 -> page 0");
    freeVal(k1);

    Value *k2; MAKE_VALUE(k2, DT_INT, 50);
    RID r2; TEST_CHECK(findKey(tree, k2, &r2));
    ASSERT_EQUALS_INT(50, r2.page, "key 50 -> page 50");
    freeVal(k2);

    TEST_CHECK(closeBTree(&tree));
    TEST_CHECK(deleteBTree("test_idx_c"));
    TEST_DONE();
}

/* ------------------------------------------------------------------ */
static void
testScanOrder(void)
{
    BTreeHandle *tree = NULL;
    testName = "btree: range scan returns sorted order";

    TEST_CHECK(createBTree("test_idx_d", DT_INT, 4));
    TEST_CHECK(openBTree(&tree, "test_idx_d"));

    /* insert in shuffled order */
    int order[] = { 7, 3, 15, 1, 9, 12, 5, 18, 0, 11, 6, 14, 2, 19, 8 };
    int n = sizeof(order) / sizeof(order[0]);
    for (int i = 0; i < n; i++) {
        Value *k; MAKE_VALUE(k, DT_INT, order[i]);
        RID r = { .page = order[i], .slot = 0 };
        TEST_CHECK(insertKey(tree, k, r));
        freeVal(k);
    }

    BT_ScanHandle *sc = NULL;
    TEST_CHECK(openTreeScan(tree, &sc));

    int prev = -1;
    RID r;
    int count = 0;
    while (nextEntry(sc, &r) == RC_OK) {
        ASSERT_TRUE(r.page > prev, "scan yields ascending keys");
        prev = r.page;
        count++;
    }
    ASSERT_EQUALS_INT(n, count, "scan returned all keys");
    TEST_CHECK(closeTreeScan(&sc));

    TEST_CHECK(closeBTree(&tree));
    TEST_CHECK(deleteBTree("test_idx_d"));
    TEST_DONE();
}

/* ------------------------------------------------------------------ */
static void
testDelete(void)
{
    BTreeHandle *tree = NULL;
    testName = "btree: delete removes keys";

    TEST_CHECK(createBTree("test_idx_e", DT_INT, 4));
    TEST_CHECK(openBTree(&tree, "test_idx_e"));

    for (int i = 0; i < 15; i++) {
        Value *k; MAKE_VALUE(k, DT_INT, i);
        RID r = { .page = i, .slot = 0 };
        TEST_CHECK(insertKey(tree, k, r));
        freeVal(k);
    }

    /* delete even keys */
    for (int i = 0; i < 15; i += 2) {
        Value *k; MAKE_VALUE(k, DT_INT, i);
        TEST_CHECK(deleteKey(tree, k));
        freeVal(k);
    }

    /* deleted keys must be gone */
    for (int i = 0; i < 15; i += 2) {
        Value *k; MAKE_VALUE(k, DT_INT, i);
        ASSERT_ERROR(findKey(tree, k, &(RID){0}), "deleted key gone");
        freeVal(k);
    }
    /* odd keys must still be present */
    for (int i = 1; i < 15; i += 2) {
        Value *k; MAKE_VALUE(k, DT_INT, i);
        RID r;
        TEST_CHECK(findKey(tree, k, &r));
        ASSERT_EQUALS_INT(i, r.page, "odd key preserved");
        freeVal(k);
    }

    int entries;
    TEST_CHECK(getNumEntries(tree, &entries));
    ASSERT_EQUALS_INT(7, entries, "7 entries after deleting 8");

    TEST_CHECK(closeBTree(&tree));
    TEST_CHECK(deleteBTree("test_idx_e"));
    TEST_DONE();
}

/* ------------------------------------------------------------------ */
static void
testDuplicate(void)
{
    BTreeHandle *tree = NULL;
    testName = "btree: duplicate insert rejected";

    TEST_CHECK(createBTree("test_idx_f", DT_INT, 4));
    TEST_CHECK(openBTree(&tree, "test_idx_f"));

    Value *k; MAKE_VALUE(k, DT_INT, 42);
    RID r = { .page = 1, .slot = 1 };
    TEST_CHECK(insertKey(tree, k, r));
    ASSERT_ERROR(insertKey(tree, k, r), "duplicate key rejected");
    freeVal(k);

    TEST_CHECK(closeBTree(&tree));
    TEST_CHECK(deleteBTree("test_idx_f"));
    TEST_DONE();
}
