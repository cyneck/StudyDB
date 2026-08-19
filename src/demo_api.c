/**
 * @file demo_api.c
 * @authors Xingli Li
 * @date 2023-07-29
 * @brief End-to-end demo: DDL + record manager + B+ tree index.
 *
 * Flow:
 *   1. init managers
 *   2. executeDDL("CREATE TABLE users (id INT, name STRING(16), age INT,
 *                  PRIMARY KEY(id))")  -> creates table file + users.idx
 *   3. open table + open index
 *   4. insert 5 records; for each, also insertKey into the B+ tree
 *   5. findKey on the index -> get RID -> getRecord  (index point lookup)
 *   6. openTreeScan -> walk all RIDs in sorted key order
 *   7. cleanup
 *
 * Build:  make all && ./build/demo
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "dberror.h"
#include "tables.h"
#include "expr.h"
#include "record_mgr.h"
#include "btree_mgr.h"
#include "ddl_parser.h"
#include "catalog.h"

/* helper: build a Record from (id, name, age) and insert into table + index */
static RC insertUser(RM_TableData *tbl, BTreeHandle *idx,
                     int id, const char *name, int age)
{
    Schema *sch = tbl->schema;
    Record *r;
    createRecord(&r, sch);

    Value *v;
    MAKE_VALUE(v, DT_INT, id);
    setAttr(r, sch, 0, v);  freeVal(v);

    MAKE_STRING_VALUE(v, name);
    setAttr(r, sch, 1, v);  freeVal(v);

    MAKE_VALUE(v, DT_INT, age);
    setAttr(r, sch, 2, v);  freeVal(v);

    /* 1. insert into the table (assigns r->id = RID) */
    insertRecord(tbl, r);

    /* 2. insert (id -> RID) into the primary-key index */
    Value *key; MAKE_VALUE(key, DT_INT, id);
    insertKey(idx, key, r->id);
    freeVal(key);

    freeRecord(r);
    return RC_OK;
}

int main(void)
{
    RM_TableData  tbl;
    BTreeHandle  *idx = NULL;

    /* ---- 1. init ---- */
    initRecordManager(NULL);
    initIndexManager(NULL);
    initCatalog();

    /* clean up any leftovers from a previous run */
    deleteTable("USERS");
    deleteBTree("USERS.idx");

    /* ---- 2. DDL: create table with primary key ---- */
    printf("== 1. executeDDL: CREATE TABLE users ==\n");
    executeDDL(
        "CREATE TABLE users ("
        "  id INT,"
        "  name STRING(16),"
        "  age INT,"
        "  PRIMARY KEY (id)"
        ")");
    /* DDL upper-cases identifiers, so the table is "USERS" and
     * the index file is "USERS.idx" */

    /* ---- 3. open table + index ---- */
    printf("== 2. open table + index ==\n");
    openTable(&tbl, "USERS");
    openBTree(&idx, "USERS.idx");

    /* ---- 4. insert records + maintain index ---- */
    printf("== 3. insert 5 users ==\n");
    insertUser(&tbl, idx, 30, "alice", 25);
    insertUser(&tbl, idx, 10, "bob",   30);
    insertUser(&tbl, idx, 50, "carol", 22);
    insertUser(&tbl, idx, 20, "dave",  28);
    insertUser(&tbl, idx, 40, "eve",   35);

    int n = getNumTuples(&tbl);
    int e;
    getNumEntries(idx, &e);
    printf("   table tuples: %d,  index entries: %d\n", n, e);

    /* ---- 5. index point lookup: find user id=20 ---- */
    printf("== 4. index lookup: WHERE id = 20 ==\n");
    Value *q; MAKE_VALUE(q, DT_INT, 20);
    RID rid;
    if (findKey(idx, q, &rid) == RC_OK) {
        Record *r;
        createRecord(&r, tbl.schema);
        getRecord(&tbl, rid, r);
        /* serialize and print */
        char *s = serializeRecord(r, tbl.schema);
        printf("   found at [%d-%d]: %s\n", rid.page, rid.slot, s);
        free(s);
        freeRecord(r);
    } else {
        printf("   not found\n");
    }
    freeVal(q);

    /* ---- 6. range scan: all users in key order ---- */
    printf("== 5. index scan: all users in id order ==\n");
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

    /* ---- 7. cleanup ---- */
    printf("== 6. cleanup ==\n");
    closeBTree(&idx);
    closeTable(&tbl);
    executeDDL("DROP TABLE users");

    shutdownIndexManager();
    shutdownCatalog();
    shutdownRecordManager();
    printf("== done ==\n");
    return 0;
}
