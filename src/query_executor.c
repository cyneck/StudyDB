/**
 * @file query_executor.c
 * @authors Xingli Li
 * @date 2023-07-29
 * @brief DML statement executor.
 *
 * Resolves column names via the catalog, then drives the record manager
 * and (if present) the B+ tree index to produce the requested effect.
 *
 * Design notes:
 * - WHERE expressions arrive from the parser with EXPR_ATTRREF nodes
 *   carrying attrRef = -1 (placeholder). resolveWhereExpr() walks the
 *   tree and fills in the real attribute index by consulting the schema.
 * - INSERT maintains the index automatically: if the table has a primary
 *   key index, the (key, RID) pair is inserted after insertRecord.
 * - DELETE removes the index entry too.
 * - UPDATE does NOT touch the index (changing a non-key column doesn't
 *   affect the index; changing a key column is rejected for simplicity).
 */
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

#include "query_executor.h"
#include "catalog.h"
#include "record_mgr.h"
#include "btree_mgr.h"
#include "expr.h"
/* serializeRecord is declared in tables.h (already included via dml_parser.h) */

/* ================================================================== */
/*  WHERE expression resolution                                      */
/* ================================================================== */

/**
 * Walk the Expr tree in-order; for each EXPR_ATTRREF with attrRef == -1,
 * match it to the next name in stmt->whereColNames and replace -1 with
 * the real attribute index from the schema.
 *
 * Returns RC_OK or RC_IM_KEY_NOT_FOUND if a column name doesn't exist.
 */
static RC
resolveExpr(Expr *expr, Schema *schema, char **colNames, int *colIdx)
{
    if (!expr) return RC_OK;

    switch (expr->type) {
        case EXPR_OP: {
            Operator *op = expr->expr.op;
            CHECKEX(resolveExpr(op->args[0], schema, colNames, colIdx));
            if (op->type != OP_BOOL_NOT)
                CHECKEX(resolveExpr(op->args[1], schema, colNames, colIdx));
            return RC_OK;
        }
        case EXPR_ATTRREF:
            if (expr->expr.attrRef == -1) {
                /* consume the next column name */
                if (*colIdx < 0) return RC_IM_KEY_NOT_FOUND;
                char *name = colNames[*colIdx];
                (*colIdx)++;
                /* find the attribute index in the schema */
                int i;
                for (i = 0; i < schema->numAttr; i++) {
                    if (strcmp(schema->attrNames[i], name) == 0) {
                        expr->expr.attrRef = i;
                        return RC_OK;
                    }
                }
                return RC_IM_KEY_NOT_FOUND;
            }
            return RC_OK;
        case EXPR_CONST:
            return RC_OK;
    }
    return RC_OK;
}

/** Wrapper: resolve all -1 placeholders in stmt->where. */
static RC
resolveWhere(DML_Statement *stmt, Schema *schema)
{
    if (!stmt->where) return RC_OK;
    int idx = 0;
    return resolveExpr(stmt->where, schema, stmt->whereColNames, &idx);
}

/* ================================================================== */
/*  find column index helper                                         */
/* ================================================================== */

static int
findColIndex(Schema *s, const char *name)
{
    for (int i = 0; i < s->numAttr; i++)
        if (strcmp(s->attrNames[i], name) == 0) return i;
    return -1;
}

/* ================================================================== */
/*  SELECT                                                           */
/* ================================================================== */

static RC
execSelect(DML_Statement *stmt, CatalogEntry *entry)
{
    RM_TableData tbl;
    CHECKEX(openTable(&tbl, entry->tableName));

    /* resolve WHERE column names */
    CHECKEX(resolveWhere(stmt, tbl.schema));

    RM_ScanHandle sc;
    Expr *cond = stmt->where;   /* NULL means scan all */
    CHECKEX(startScan(&tbl, &sc, cond));

    Record *r;
    CHECKEX(createRecord(&r, tbl.schema));

    /* print header */
    printf("--- SELECT from %s ---\n", entry->tableName);
    int rc;
    int rowCount = 0;
    while ((rc = next(&sc, r)) == RC_OK) {
        char *s = serializeRecord(r, tbl.schema);
        printf("  %s\n", s);
        free(s);
        rowCount++;
    }
    printf("--- %d row(s) ---\n", rowCount);

    freeRecord(r);
    closeScan(&sc);
    closeTable(&tbl);
    return RC_OK;
}

/* ================================================================== */
/*  INSERT                                                           */
/* ================================================================== */

static RC
execInsert(DML_Statement *stmt, CatalogEntry *entry)
{
    RM_TableData tbl;
    CHECKEX(openTable(&tbl, entry->tableName));

    Schema *sch = tbl.schema;
    if (stmt->numValues != sch->numAttr) {
        closeTable(&tbl);
        return RC_IM_INCOMPATIBLE_DATA;
    }

    Record *r;
    CHECKEX(createRecord(&r, sch));

    /* set each attribute */
    for (int i = 0; i < stmt->numValues; i++) {
        CHECKEX(setAttr(r, sch, i, stmt->values[i]));
    }

    /* insert into table */
    CHECKEX(insertRecord(&tbl, r));

    /* maintain index if present */
    if (entry->hasIndex && entry->indexName) {
        BTreeHandle *idx = NULL;
        if (openBTree(&idx, entry->indexName) == RC_OK) {
            /* the indexed key is the primary key attribute */
            int keyAttr = sch->keyAttrs[0];
            Value *kv;
            CHECKEX(getAttr(r, sch, keyAttr, &kv));
            insertKey(idx, kv, r->id);
            freeVal(kv);
            closeBTree(&idx);
        }
    }

    freeRecord(r);
    closeTable(&tbl);
    return RC_OK;
}

/* ================================================================== */
/*  UPDATE                                                           */
/* ================================================================== */

static RC
execUpdate(DML_Statement *stmt, CatalogEntry *entry)
{
    RM_TableData tbl;
    CHECKEX(openTable(&tbl, entry->tableName));
    CHECKEX(resolveWhere(stmt, tbl.schema));

    /* resolve SET column names to indices */
    int *setIdx = (int *) malloc(sizeof(int) * stmt->numSets);
    for (int i = 0; i < stmt->numSets; i++) {
        setIdx[i] = findColIndex(tbl.schema, stmt->setCols[i]);
        if (setIdx[i] < 0) {
            free(setIdx);
            closeTable(&tbl);
            return RC_IM_KEY_NOT_FOUND;
        }
        /* reject updating the primary key column (index would break) */
        for (int k = 0; k < tbl.schema->keySize; k++) {
            if (setIdx[i] == tbl.schema->keyAttrs[k]) {
                free(setIdx);
                closeTable(&tbl);
                fprintf(stderr, "[executor] updating primary key column is not supported\n");
                return RC_IM_INCOMPATIBLE_DATA;
            }
        }
    }

    RM_ScanHandle sc;
    CHECKEX(startScan(&tbl, &sc, stmt->where));

    Record *r;
    CHECKEX(createRecord(&r, tbl.schema));

    int updated = 0;
    int rc;
    while ((rc = next(&sc, r)) == RC_OK) {
        for (int i = 0; i < stmt->numSets; i++) {
            setAttr(r, tbl.schema, setIdx[i], stmt->setVals[i]);
        }
        CHECKEX(updateRecord(&tbl, r));
        updated++;
    }
    printf("--- UPDATE %s: %d row(s) ---\n", entry->tableName, updated);

    free(setIdx);
    freeRecord(r);
    closeScan(&sc);
    closeTable(&tbl);
    return RC_OK;
}

/* ================================================================== */
/*  DELETE                                                           */
/* ================================================================== */

static RC
execDelete(DML_Statement *stmt, CatalogEntry *entry)
{
    RM_TableData tbl;
    CHECKEX(openTable(&tbl, entry->tableName));
    CHECKEX(resolveWhere(stmt, tbl.schema));

    RM_ScanHandle sc;
    CHECKEX(startScan(&tbl, &sc, stmt->where));

    Record *r;
    CHECKEX(createRecord(&r, tbl.schema));

    int deleted = 0;
    int rc;
    while ((rc = next(&sc, r)) == RC_OK) {
        RID rid = r->id;
        /* remove from index first (before the record is gone) */
        if (entry->hasIndex && entry->indexName) {
            BTreeHandle *idx = NULL;
            if (openBTree(&idx, entry->indexName) == RC_OK) {
                int keyAttr = tbl.schema->keyAttrs[0];
                Value *kv;
                getAttr(r, tbl.schema, keyAttr, &kv);
                deleteKey(idx, kv);
                freeVal(kv);
                closeBTree(&idx);
            }
        }
        CHECKEX(deleteRecord(&tbl, rid));
        deleted++;
    }
    printf("--- DELETE from %s: %d row(s) ---\n", entry->tableName, deleted);

    freeRecord(r);
    closeScan(&sc);
    closeTable(&tbl);
    return RC_OK;
}

/* ================================================================== */
/*  Dispatch                                                         */
/* ================================================================== */

RC
executeDML(DML_Statement *stmt)
{
    if (!stmt) return RC_NULL_POINTER;

    /* look up the table in the catalog */
    CatalogEntry *entry = catalogLookupTable(stmt->tableName);
    if (!entry) {
        fprintf(stderr, "[executor] table '%s' not found in catalog\n",
                stmt->tableName);
        return RC_IM_KEY_NOT_FOUND;
    }

    switch (stmt->type) {
        case DML_SELECT: return execSelect(stmt, entry);
        case DML_INSERT: return execInsert(stmt, entry);
        case DML_UPDATE: return execUpdate(stmt, entry);
        case DML_DELETE: return execDelete(stmt, entry);
        default:          return RC_IM_INCOMPATIBLE_DATA;
    }
}

RC
executeSQL(const char *sql)
{
    DML_Statement *stmt = NULL;
    RC rc = parseDML(sql, &stmt);
    if (rc != RC_OK) return rc;
    rc = executeDML(stmt);
    freeDMLStatement(stmt);
    return rc;
}
