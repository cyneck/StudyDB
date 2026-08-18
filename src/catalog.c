/**
 * @file catalog.c
 * @authors Xingli Li
 * @date 2023-07-29
 * @brief System catalog implementation.
 *
 * Catalog entries persist to "catalog.bin" through the storage manager.
 * Schema allocation helpers are shared with the record manager, while
 * openTable restores its schema independently from the table file.
 *
 * Serialisation format (one entry, laid out contiguously in a byte buffer):
 *   tableNameLen(int) + tableName(bytes)
 *   numAttr(int) + keySize(int)
 *   for each attr:  nameLen(int) + name(bytes) + dataType(int) + typeLength(int)
 *   for each key:   keyAttr(int)
 *   hasIndex(int)
 *   indexNameLen(int) + indexName(bytes)
 *
 * File layout:
 *   Page 0:  [0..3] = entry count
 *   Page 1+: serialised entries, packed back-to-back, spilling across pages.
 */
#define _POSIX_C_SOURCE 200809L
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

#include "catalog.h"
#include "storage_mgr.h"
#include "record_mgr.h"   /* createSchema / freeSchema */

/* ---- global catalog instance ---- */
static Catalog g_catalog;
static SM_FileHandle g_catFH;
static int g_initialized = 0;

/* ================================================================== */
/*  Serialisation helpers                                            */
/* ================================================================== */

/** Compute the serialised size of one entry. */
static int
entrySize(const CatalogEntry *e)
{
    int sz = 0;
    sz += sizeof(int) + (int) strlen(e->tableName);
    sz += sizeof(int) + sizeof(int);            /* numAttr, keySize */
    for (int i = 0; i < e->schema->numAttr; i++) {
        sz += sizeof(int) + (int) strlen(e->schema->attrNames[i]);
        sz += sizeof(int) + sizeof(int);        /* dataType, typeLength */
    }
    sz += e->schema->keySize * sizeof(int);     /* keyAttrs */
    sz += sizeof(int);                          /* hasIndex */
    if (e->hasIndex && e->indexName) {
        sz += sizeof(int) + (int) strlen(e->indexName);
    } else {
        sz += sizeof(int);                      /* indexNameLen = 0 */
    }
    return sz;
}

/** Serialise one entry into buf at *off; advance *off. */
static void
entrySerialize(char *buf, int *off, const CatalogEntry *e)
{
    int len;
    Schema *s = e->schema;

    len = (int) strlen(e->tableName);
    memcpy(buf + *off, &len, sizeof(int)); *off += sizeof(int);
    memcpy(buf + *off, e->tableName, len); *off += len;

    memcpy(buf + *off, &s->numAttr,  sizeof(int)); *off += sizeof(int);
    memcpy(buf + *off, &s->keySize,  sizeof(int)); *off += sizeof(int);

    for (int i = 0; i < s->numAttr; i++) {
        len = (int) strlen(s->attrNames[i]);
        memcpy(buf + *off, &len, sizeof(int)); *off += sizeof(int);
        memcpy(buf + *off, s->attrNames[i], len); *off += len;
        memcpy(buf + *off, &s->dataTypes[i],  sizeof(int)); *off += sizeof(int);
        memcpy(buf + *off, &s->typeLength[i], sizeof(int)); *off += sizeof(int);
    }
    for (int i = 0; i < s->keySize; i++) {
        memcpy(buf + *off, &s->keyAttrs[i], sizeof(int)); *off += sizeof(int);
    }

    memcpy(buf + *off, &e->hasIndex, sizeof(int)); *off += sizeof(int);

    if (e->hasIndex && e->indexName) {
        len = (int) strlen(e->indexName);
        memcpy(buf + *off, &len, sizeof(int)); *off += sizeof(int);
        memcpy(buf + *off, e->indexName, len); *off += len;
    } else {
        int zero = 0;
        memcpy(buf + *off, &zero, sizeof(int)); *off += sizeof(int);
    }
}

/** Deserialise one entry from buf at *off; advance *off. Returns malloc'd entry. */
static CatalogEntry *
entryDeserialize(const char *buf, int *off)
{
    CatalogEntry *e = (CatalogEntry *) calloc(1, sizeof(CatalogEntry));
    int len;

    memcpy(&len, buf + *off, sizeof(int)); *off += sizeof(int);
    e->tableName = (char *) malloc(len + 1);
    memcpy(e->tableName, buf + *off, len); e->tableName[len] = '\0'; *off += len;

    Schema *s = (Schema *) calloc(1, sizeof(Schema));
    memcpy(&s->numAttr, buf + *off, sizeof(int)); *off += sizeof(int);
    memcpy(&s->keySize, buf + *off, sizeof(int)); *off += sizeof(int);

    s->attrNames  = (char **)     malloc(sizeof(char *) * s->numAttr);
    s->dataTypes  = (DataType *)  malloc(sizeof(DataType) * s->numAttr);
    s->typeLength = (int *)       malloc(sizeof(int) * s->numAttr);
    s->keyAttrs   = (int *)       malloc(sizeof(int) * (s->keySize > 0 ? s->keySize : 1));

    for (int i = 0; i < s->numAttr; i++) {
        memcpy(&len, buf + *off, sizeof(int)); *off += sizeof(int);
        s->attrNames[i] = (char *) malloc(len + 1);
        memcpy(s->attrNames[i], buf + *off, len);
        s->attrNames[i][len] = '\0'; *off += len;
        memcpy(&s->dataTypes[i],  buf + *off, sizeof(int)); *off += sizeof(int);
        memcpy(&s->typeLength[i], buf + *off, sizeof(int)); *off += sizeof(int);
    }
    for (int i = 0; i < s->keySize; i++) {
        memcpy(&s->keyAttrs[i], buf + *off, sizeof(int)); *off += sizeof(int);
    }
    e->schema = s;

    memcpy(&e->hasIndex, buf + *off, sizeof(int)); *off += sizeof(int);

    memcpy(&len, buf + *off, sizeof(int)); *off += sizeof(int);
    if (len > 0) {
        e->indexName = (char *) malloc(len + 1);
        memcpy(e->indexName, buf + *off, len); e->indexName[len] = '\0'; *off += len;
    } else {
        e->indexName = NULL;
    }

    e->next = NULL;
    return e;
}

/* ================================================================== */
/*  Persistence: flush the in-memory list to catalog.bin             */
/* ================================================================== */

static RC
catalogFlush(void)
{
    /* compute total size */
    int total = sizeof(int);     /* count */
    CatalogEntry *e = g_catalog.head;
    while (e) {
        total += entrySize(e);
        e = e->next;
    }
    int numPages = (total - 1) / PAGE_SIZE + 1;
    if (numPages < 1) numPages = 1;

    char *buf = (char *) calloc(numPages, PAGE_SIZE);
    int off = 0;
    memcpy(buf + off, &g_catalog.count, sizeof(int)); off += sizeof(int);
    e = g_catalog.head;
    while (e) {
        entrySerialize(buf, &off, e);
        e = e->next;
    }

    /* ensure file has enough pages */
    ensureCapacity(numPages, &g_catFH);
    g_catFH.curPagePos = 0;
    for (int i = 0; i < numPages; i++) {
        writeBlock(i, &g_catFH, buf + i * PAGE_SIZE);
    }
    free(buf);
    return RC_OK;
}

/* ================================================================== */
/*  Lifecycle                                                        */
/* ================================================================== */

RC
initCatalog(void)
{
    if (g_initialized) return RC_OK;
    initStorageManager();

    g_catalog.head = NULL;
    g_catalog.count = 0;
    g_catalog.pageFile = strdup("catalog.bin");

    /* if catalog.bin doesn't exist, create it; else load */
    if (openPageFile(g_catalog.pageFile, &g_catFH) != RC_OK) {
        CHECKEX(createPageFile(g_catalog.pageFile));
        CHECKEX(openPageFile(g_catalog.pageFile, &g_catFH));
        g_initialized = 1;
        catalogFlush();      /* write count=0 */
        return RC_OK;
    }

    /* load entries from disk */
    char *buf = (char *) malloc(g_catFH.totalNumPages * PAGE_SIZE);
    for (int i = 0; i < g_catFH.totalNumPages; i++) {
        readBlock(i, &g_catFH, buf + i * PAGE_SIZE);
    }

    int off = 0;
    memcpy(&g_catalog.count, buf + off, sizeof(int)); off += sizeof(int);

    CatalogEntry *tail = NULL;
    for (int i = 0; i < g_catalog.count; i++) {
        CatalogEntry *e = entryDeserialize(buf, &off);
        if (tail == NULL) g_catalog.head = e;
        else tail->next = e;
        tail = e;
    }
    free(buf);
    g_initialized = 1;
    return RC_OK;
}

RC
shutdownCatalog(void)
{
    if (!g_initialized) return RC_OK;
    catalogFlush();
    closePageFile(&g_catFH);

    /* free the in-memory list */
    CatalogEntry *e = g_catalog.head;
    while (e) {
        CatalogEntry *next = e->next;
        free(e->tableName);
        if (e->schema) freeSchema(e->schema);
        if (e->indexName) free(e->indexName);
        free(e);
        e = next;
    }
    free(g_catalog.pageFile);
    g_catalog.head = NULL;
    g_catalog.count = 0;
    g_initialized = 0;
    return RC_OK;
}

/* ================================================================== */
/*  Registration / lookup                                            */
/* ================================================================== */

RC
catalogRegisterTable(const char *tableName, Schema *schema,
                     int hasIndex, const char *indexName)
{
    if (!g_initialized) return RC_RM_MANAGER_CLOSED;

    /* duplicate check */
    if (catalogLookupTable(tableName) != NULL)
        return RC_RM_TABLE_EXISTS;

    CatalogEntry *e = (CatalogEntry *) calloc(1, sizeof(CatalogEntry));
    e->tableName = strdup(tableName);
    /* deep-copy attrNames: createSchema stores the char* pointers directly
     * (it does NOT copy the strings), so we must allocate our own copies.
     * Otherwise the caller's freeSchema() would leave dangling pointers
     * in the catalog → use-after-free → heap corruption. */
    char **attrNamesCopy = (char **) malloc(sizeof(char *) * schema->numAttr);
    for (int i = 0; i < schema->numAttr; i++) {
        attrNamesCopy[i] = strdup(schema->attrNames[i]);
    }
    e->schema = createSchema(schema->numAttr, attrNamesCopy,
                             schema->dataTypes, schema->typeLength,
                             schema->keySize, schema->keyAttrs);
    /* createSchema copied attrNamesCopy pointers into its own array;
     * free the temporary array container (NOT the strings — those are
     * now owned by the schema). dataTypes/typeLength/keyAttrs were
     * copied by value, so the caller's originals are safe to free. */
    free(attrNamesCopy);
    e->hasIndex = hasIndex;
    e->indexName = indexName ? strdup(indexName) : NULL;
    e->next = NULL;

    /* append to list */
    if (g_catalog.head == NULL) g_catalog.head = e;
    else {
        CatalogEntry *t = g_catalog.head;
        while (t->next) t = t->next;
        t->next = e;
    }
    g_catalog.count++;
    catalogFlush();
    return RC_OK;
}

RC
catalogDropTable(const char *tableName)
{
    if (!g_initialized) return RC_RM_MANAGER_CLOSED;

    CatalogEntry *prev = NULL, *cur = g_catalog.head;
    while (cur) {
        if (strcmp(cur->tableName, tableName) == 0) {
            if (prev) prev->next = cur->next;
            else g_catalog.head = cur->next;
            free(cur->tableName);
            if (cur->schema) freeSchema(cur->schema);
            if (cur->indexName) free(cur->indexName);
            free(cur);
            g_catalog.count--;
            catalogFlush();
            return RC_OK;
        }
        prev = cur;
        cur = cur->next;
    }
    return RC_IM_KEY_NOT_FOUND;   /* table not in catalog */
}

CatalogEntry *
catalogLookupTable(const char *tableName)
{
    if (!g_initialized) return NULL;
    CatalogEntry *e = g_catalog.head;
    while (e) {
        if (strcmp(e->tableName, tableName) == 0) return e;
        e = e->next;
    }
    return NULL;
}

RC
catalogListTables(char ***names, int *count)
{
    if (!g_initialized) return RC_RM_MANAGER_CLOSED;
    *count = g_catalog.count;
    if (g_catalog.count == 0) { *names = NULL; return RC_OK; }
    *names = (char **) malloc(sizeof(char *) * g_catalog.count);
    int i = 0;
    CatalogEntry *e = g_catalog.head;
    while (e) {
        (*names)[i++] = strdup(e->tableName);
        e = e->next;
    }
    return RC_OK;
}

void
catalogPrint(void)
{
    printf("Catalog (%d tables):\n", g_catalog.count);
    CatalogEntry *e = g_catalog.head;
    while (e) {
        char *s = serializeSchema(e->schema);
        printf("  %s  idx=%s  %s", e->tableName,
               e->hasIndex ? e->indexName : "none", s);
        free(s);
        e = e->next;
    }
}
