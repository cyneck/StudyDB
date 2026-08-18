/**
 * @file test_dml.c
 * @authors Xingli Li
 * @date 2023-07-29
 * @brief DML parser + executor + catalog integration tests.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "dberror.h"
#include "tables.h"
#include "expr.h"
#include "record_mgr.h"
#include "btree_mgr.h"
#include "catalog.h"
#include "ddl_parser.h"
#include "dml_parser.h"
#include "query_executor.h"
#include "test_helper.h"

static void testParseSelect(void);
static void testParseInsert(void);
static void testParseUpdateDelete(void);
static void testExecuteInsertSelect(void);
static void testWhereFilter(void);
static void testCatalogIntegration(void);

char *testName;

int
main(void)
{
    testName = "";
    initRecordManager(NULL);
    initIndexManager(NULL);
    initCatalog();

    testParseSelect();
    testParseInsert();
    testParseUpdateDelete();
    testExecuteInsertSelect();
    testWhereFilter();
    testCatalogIntegration();

    shutdownCatalog();
    shutdownIndexManager();
    shutdownRecordManager();
    printf("ALL DML TESTS PASSED\n");
    return 0;
}

/* ------------------------------------------------------------------ */
static void
testParseSelect(void)
{
    testName = "dml: parse SELECT";
    DML_Statement *st = NULL;

    TEST_CHECK(parseDML("SELECT * FROM users WHERE age < 30 ;", &st));
    ASSERT_EQUALS_INT(DML_SELECT, (int) st->type, "type SELECT");
    ASSERT_EQUALS_STRING("USERS", st->tableName, "table name");
    ASSERT_TRUE(st->selectAll == 1, "SELECT *");
    ASSERT_TRUE(st->where != NULL, "has WHERE");
    freeDMLStatement(st);

    TEST_CHECK(parseDML("SELECT id, name FROM users;", &st));
    ASSERT_EQUALS_INT(2, st->numCols, "2 columns");
    ASSERT_EQUALS_STRING("ID", st->columns[0], "col0");
    ASSERT_EQUALS_STRING("NAME", st->columns[1], "col1");
    ASSERT_TRUE(st->where == NULL, "no WHERE");
    freeDMLStatement(st);

    TEST_DONE();
}

/* ------------------------------------------------------------------ */
static void
testParseInsert(void)
{
    testName = "dml: parse INSERT";
    DML_Statement *st = NULL;

    TEST_CHECK(parseDML("INSERT INTO users VALUES (1, 'alice', 25);", &st));
    ASSERT_EQUALS_INT(DML_INSERT, (int) st->type, "type INSERT");
    ASSERT_EQUALS_INT(3, st->numValues, "3 values");
    ASSERT_EQUALS_INT(DT_INT, (int) st->values[0]->dt, "val0 INT");
    ASSERT_EQUALS_INT(1, st->values[0]->v.intV, "val0 = 1");
    ASSERT_EQUALS_INT(DT_STRING, (int) st->values[1]->dt, "val1 STRING");
    ASSERT_EQUALS_STRING("alice", st->values[1]->v.stringV, "val1 = alice");
    freeDMLStatement(st);

    TEST_DONE();
}

/* ------------------------------------------------------------------ */
static void
testParseUpdateDelete(void)
{
    testName = "dml: parse UPDATE and DELETE";
    DML_Statement *st = NULL;

    TEST_CHECK(parseDML("UPDATE users SET age = 30 WHERE id = 1;", &st));
    ASSERT_EQUALS_INT(DML_UPDATE, (int) st->type, "type UPDATE");
    ASSERT_EQUALS_INT(1, st->numSets, "1 SET");
    ASSERT_EQUALS_STRING("AGE", st->setCols[0], "set col");
    ASSERT_EQUALS_INT(30, st->setVals[0]->v.intV, "set val");
    freeDMLStatement(st);

    TEST_CHECK(parseDML("DELETE FROM users WHERE id = 1;", &st));
    ASSERT_EQUALS_INT(DML_DELETE, (int) st->type, "type DELETE");
    ASSERT_TRUE(st->where != NULL, "has WHERE");
    freeDMLStatement(st);

    TEST_DONE();
}

/* ------------------------------------------------------------------ */
static void
testExecuteInsertSelect(void)
{
    testName = "dml: execute INSERT + SELECT";

    /* clean slate */
    deleteTable("T1");
    deleteBTree("T1.idx");
    catalogDropTable("T1");

    executeDDL("CREATE TABLE T1 (a INT, b STRING(8), PRIMARY KEY(a))");

    /* T1 only has 2 columns (a, b); inserting 3 values should fail */
    ASSERT_ERROR(executeSQL("INSERT INTO T1 VALUES (1, 'one', 0)"),
                 "3 values into 2-col table should fail");

    /* re-create with 3 columns */
    deleteTable("T1");
    deleteBTree("T1.idx");
    catalogDropTable("T1");
    executeDDL("CREATE TABLE T1 (a INT, b STRING(8), c INT, PRIMARY KEY(a))");

    TEST_CHECK(executeSQL("INSERT INTO T1 VALUES (10, 'ten', 100)"));
    TEST_CHECK(executeSQL("INSERT INTO T1 VALUES (20, 'twenty', 200)"));
    TEST_CHECK(executeSQL("INSERT INTO T1 VALUES (30, 'thirty', 300)"));

    /* SELECT * should print all 3 rows */
    printf("  [expecting 3 rows below]\n");
    TEST_CHECK(executeSQL("SELECT * FROM T1"));

    executeDDL("DROP TABLE T1");
    TEST_DONE();
}

/* ------------------------------------------------------------------ */
static void
testWhereFilter(void)
{
    testName = "dml: WHERE filter";

    deleteTable("T2");
    deleteBTree("T2.idx");
    catalogDropTable("T2");
    executeDDL("CREATE TABLE T2 (id INT, val INT, PRIMARY KEY(id))");

    for (int i = 1; i <= 5; i++) {
        char sql[128];
        snprintf(sql, sizeof(sql), "INSERT INTO T2 VALUES (%d, %d)", i, i * 10);
        TEST_CHECK(executeSQL(sql));
    }

    /* WHERE val < 30 → should match id=1(val=10), id=2(val=20) → 2 rows */
    printf("  [expecting 2 rows with val < 30]\n");
    TEST_CHECK(executeSQL("SELECT * FROM T2 WHERE val < 30"));

    executeDDL("DROP TABLE T2");
    TEST_DONE();
}

/* ------------------------------------------------------------------ */
static void
testCatalogIntegration(void)
{
    testName = "dml: catalog integration";

    deleteTable("CATTEST");
    deleteBTree("CATTEST.idx");
    catalogDropTable("CATTEST");

    /* CREATE TABLE should auto-register in catalog */
    executeDDL("CREATE TABLE CATTEST (k INT, v STRING(4), PRIMARY KEY(k))");

    CatalogEntry *e = catalogLookupTable("CATTEST");
    ASSERT_TRUE(e != NULL, "table found in catalog");
    ASSERT_TRUE(e->hasIndex == 1, "catalog knows about index");
    ASSERT_EQUALS_INT(2, e->schema->numAttr, "catalog has schema");

    /* DROP TABLE should auto-deregister */
    executeDDL("DROP TABLE CATTEST");
    e = catalogLookupTable("CATTEST");
    ASSERT_TRUE(e == NULL, "table removed from catalog after DROP");

    TEST_DONE();
}
