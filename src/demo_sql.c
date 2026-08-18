/**
 * @file demo_sql.c
 * @authors Xingli Li
 * @date 2023-07-29
 * @brief End-to-end SQL demo: DDL + DML + Catalog integration.
 *
 * This demo shows a full SQL workflow:
 *   1. init all managers (record + index + catalog)
 *   2. CREATE TABLE with primary key  → auto-registers in catalog
 *   3. INSERT several rows via SQL    → auto-maintains B+ tree index
 *   4. SELECT *                       → linear scan
 *   5. SELECT ... WHERE age < 26      → scan + filter
 *   6. UPDATE ... SET age=26 WHERE id=20
 *   7. DELETE FROM ... WHERE id=10    → removes index entry too
 *   8. SELECT *                       → verify final state
 *   9. catalogPrint()                 → show catalog knows the table
 *  10. DROP TABLE                     → auto-drops index + catalog entry
 *
 * Build:  make all && ./build/demo_sql
 */
#include <stdio.h>
#include <stdlib.h>

#include "dberror.h"
#include "record_mgr.h"
#include "btree_mgr.h"
#include "catalog.h"
#include "ddl_parser.h"
#include "query_executor.h"

static void run(const char *sql) {
    printf("\n>>> %s\n", sql);
    RC rc = executeSQL(sql);
    if (rc != RC_OK)
        printf("  [error %d]\n", rc);
}

int main(void)
{
    /* 1. init */
    initRecordManager(NULL);
    initIndexManager(NULL);
    initCatalog();

    /* clean up leftovers */
    deleteTable("USERS");
    deleteBTree("USERS.idx");

    /* 2. DDL: create table with primary key */
    printf("=== 1. CREATE TABLE ===\n");
    executeDDL(
        "CREATE TABLE users ("
        "  id INT,"
        "  name STRING(16),"
        "  age INT,"
        "  PRIMARY KEY (id)"
        ")");

    /* 3. INSERT via SQL */
    printf("\n=== 2. INSERT rows ===\n");
    run("INSERT INTO users VALUES (30, 'alice', 25)");
    run("INSERT INTO users VALUES (10, 'bob', 30)");
    run("INSERT INTO users VALUES (20, 'carol', 22)");
    run("INSERT INTO users VALUES (40, 'dave', 28)");

    /* 4. SELECT * */
    printf("\n=== 3. SELECT * FROM users ===\n");
    run("SELECT * FROM users");

    /* 5. SELECT with WHERE */
    printf("\n=== 4. SELECT WHERE age < 26 ===\n");
    run("SELECT * FROM users WHERE age < 26");

    /* 6. UPDATE */
    printf("\n=== 5. UPDATE carol's age ===\n");
    run("UPDATE users SET age = 26 WHERE id = 20");

    /* 7. DELETE */
    printf("\n=== 6. DELETE bob ===\n");
    run("DELETE FROM users WHERE id = 10");

    /* 8. SELECT * to verify */
    printf("\n=== 7. SELECT * (final state) ===\n");
    run("SELECT * FROM users");

    /* 9. catalog */
    printf("\n=== 8. Catalog ===\n");
    catalogPrint();

    /* 10. DROP */
    printf("\n=== 9. DROP TABLE ===\n");
    executeDDL("DROP TABLE users");

    /* cleanup */
    shutdownCatalog();
    shutdownIndexManager();
    shutdownRecordManager();
    printf("\n=== done ===\n");
    return 0;
}
