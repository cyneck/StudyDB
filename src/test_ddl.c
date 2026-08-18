/**
 * @file test_ddl.c
 * @authors Xingli Li
 * @date 2023-07-29
 * @brief DDL parser unit tests.
 *
 * Covers: CREATE TABLE with several types and a PRIMARY KEY, DROP TABLE,
 * error on unknown type, and an end-to-end executeDDL run that materialises
 * a table file (and its .idx index file when a PK is declared).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "dberror.h"
#include "tables.h"
#include "record_mgr.h"
#include "btree_mgr.h"
#include "catalog.h"
#include "ddl_parser.h"
#include "test_helper.h"

static void testParseCreate(void);
static void testParseCreateWithPK(void);
static void testParseDrop(void);
static void testParseError(void);
static void testExecuteCreateDrop(void);

char *testName;

int
main(void)
{
    testName = "";
    initRecordManager(NULL);
    initIndexManager(NULL);
    initCatalog();

    testParseCreate();
    testParseCreateWithPK();
    testParseDrop();
    testParseError();
    testExecuteCreateDrop();

    shutdownCatalog();
    shutdownIndexManager();
    shutdownRecordManager();
    printf("ALL DDL TESTS PASSED\n");
    return 0;
}

/* ------------------------------------------------------------------ */
static void
testParseCreate(void)
{
    testName = "ddl: parse CREATE TABLE without PK";
    DDL_Statement *st = NULL;

    const char *sql = "CREATE TABLE students ("
                      "  id INT,"
                      "  name STRING(20),"
                      "  gpa FLOAT,"
                      "  active BOOL"
                      ");";
    TEST_CHECK(parseDDL(sql, &st));
    ASSERT_EQUALS_INT(DDL_CREATE_TABLE, (int) st->type, "type is CREATE");
    ASSERT_EQUALS_STRING("STUDENTS", st->tableName, "table name upper-cased");
    ASSERT_EQUALS_INT(4, st->schema->numAttr, "4 columns");
    ASSERT_EQUALS_INT(DT_INT,    (int) st->schema->dataTypes[0], "col0 INT");
    ASSERT_EQUALS_INT(DT_STRING, (int) st->schema->dataTypes[1], "col1 STRING");
    ASSERT_EQUALS_INT(DT_FLOAT,  (int) st->schema->dataTypes[2], "col2 FLOAT");
    ASSERT_EQUALS_INT(DT_BOOL,   (int) st->schema->dataTypes[3], "col3 BOOL");
    ASSERT_EQUALS_INT(20, st->schema->typeLength[1], "STRING(20)");
    ASSERT_EQUALS_INT(-1, st->primaryKeyAttr, "no PK declared");

    freeDDLStatement(st);
    TEST_DONE();
}

/* ------------------------------------------------------------------ */
static void
testParseCreateWithPK(void)
{
    testName = "ddl: parse CREATE TABLE with PRIMARY KEY";
    DDL_Statement *st = NULL;

    const char *sql = "CREATE TABLE accounts ("
                      "  acct INT,"
                      "  balance FLOAT,"
                      "  PRIMARY KEY (acct)"
                      ")";
    TEST_CHECK(parseDDL(sql, &st));
    ASSERT_EQUALS_INT(0, st->primaryKeyAttr, "PK on first column");
    ASSERT_EQUALS_INT(1, st->schema->keySize, "keySize == 1");
    ASSERT_EQUALS_INT(0, st->schema->keyAttrs[0], "keyAttrs[0] == 0");

    freeDDLStatement(st);
    TEST_DONE();
}

/* ------------------------------------------------------------------ */
static void
testParseDrop(void)
{
    testName = "ddl: parse DROP TABLE";
    DDL_Statement *st = NULL;

    TEST_CHECK(parseDDL("drop table old_data;", &st));
    ASSERT_EQUALS_INT(DDL_DROP_TABLE, (int) st->type, "type is DROP");
    ASSERT_EQUALS_STRING("OLD_DATA", st->tableName, "drop table name");
    ASSERT_TRUE(st->schema == NULL, "DROP has no schema");

    freeDDLStatement(st);
    TEST_DONE();
}

/* ------------------------------------------------------------------ */
static void
testParseError(void)
{
    testName = "ddl: parse error on unknown type";
    DDL_Statement *st = NULL;

    /* BLOB is not a supported type */
    ASSERT_ERROR(parseDDL("CREATE TABLE bad (x BLOB)", &st),
                 "unknown type rejected");
    ASSERT_TRUE(st == NULL, "no statement returned on error");
    TEST_DONE();
}

/* ------------------------------------------------------------------ */
static void
testExecuteCreateDrop(void)
{
    testName = "ddl: execute CREATE then DROP round-trip";

    /* clean up any leftover files from a previous failed run */
    deleteTable("DDLDEMO");
    deleteBTree("DDLDEMO.idx");

    TEST_CHECK(executeDDL(
        "CREATE TABLE ddldemo ("
        "  k INT,"
        "  v STRING(8),"
        "  PRIMARY KEY (k)"
        ")"));

    /* the table file should exist now; opening it must succeed */
    RM_TableData *tbl = (RM_TableData *) malloc(sizeof(RM_TableData));
    TEST_CHECK(openTable(tbl, "DDLDEMO"));
    TEST_CHECK(closeTable(tbl));
    free(tbl);

    /* the index file <name>.idx should also exist (PK declared) */
    BTreeHandle *bt = NULL;
    TEST_CHECK(openBTree(&bt, "DDLDEMO.idx"));
    DataType kt;
    TEST_CHECK(getKeyType(bt, &kt));
    ASSERT_EQUALS_INT(DT_INT, (int) kt, "index key type INT");
    TEST_CHECK(closeBTree(&bt));

    /* DROP removes the table and best-effort deletes the index file */
    TEST_CHECK(executeDDL("DROP TABLE ddldemo"));

    /* opening the deleted table should now fail */
    RM_TableData *tbl2 = (RM_TableData *) malloc(sizeof(RM_TableData));
    ASSERT_ERROR(openTable(tbl2, "DDLDEMO"), "table gone after DROP");
    free(tbl2);

    TEST_DONE();
}
