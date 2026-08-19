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
 * File layout (packed across pages):
 *   magic(int) + formatVersion(int) + entryCount(int) + entries...
 * Deserialization validates every count, length, type, key index, and bound.
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

#define CATALOG_MAGIC 0x53444243 /* "SDBC" */
#define CATALOG_VERSION 1
#define CATALOG_MAX_ENTRIES 10000
#define CATALOG_MAX_ATTRS 1024

static int
readIntChecked(const char *buf, int size, int *off, int *value)
{
    if (*off < 0 || *off > size - (int)sizeof(int)) return 0;
    memcpy(value, buf + *off, sizeof(int));
    *off += sizeof(int);
    return 1;
}

static int
readStringChecked(const char *buf, int size, int *off, char **value)
{
    int len;
    if (!readIntChecked(buf, size, off, &len) || len < 0 || len > size - *off)
        return 0;
    char *s = (char *)malloc((size_t)len + 1);
    if (s == NULL) return 0;
    memcpy(s, buf + *off, len);
    s[len] = '\0';
    *off += len;
    *value = s;
    return 1;
}

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

/** Deserialise one entry with bounds checking. */
static RC
entryDeserialize(const char *buf, int size, int *off, CatalogEntry **result)
{
    CatalogEntry *e = (CatalogEntry *) calloc(1, sizeof(CatalogEntry));
    if (e == NULL) return RC_ALLOCATION_FAILED;
    if (!readStringChecked(buf, size, off, &e->tableName)) goto invalid;

    Schema *s = (Schema *) calloc(1, sizeof(Schema));
    if (s == NULL) goto allocation;
    if (!readIntChecked(buf, size, off, &s->numAttr) ||
        !readIntChecked(buf, size, off, &s->keySize) ||
        s->numAttr <= 0 || s->numAttr > CATALOG_MAX_ATTRS ||
        s->keySize < 0 || s->keySize > s->numAttr) {
        free(s);
        goto invalid;
    }

    s->attrNames  = (char **)calloc((size_t)s->numAttr, sizeof(char *));
    s->dataTypes  = (DataType *)malloc(sizeof(DataType) * (size_t)s->numAttr);
    s->typeLength = (int *)malloc(sizeof(int) * (size_t)s->numAttr);
    s->keyAttrs   = (int *)malloc(sizeof(int) * (size_t)(s->keySize > 0 ? s->keySize : 1));
    if (!s->attrNames || !s->dataTypes || !s->typeLength || !s->keyAttrs) {
        e->schema = s;
        goto allocation;
    }
    e->schema = s;

    for (int i = 0; i < s->numAttr; i++) {
        int dt;
        if (!readStringChecked(buf, size, off, &s->attrNames[i]) ||
            !readIntChecked(buf, size, off, &dt) ||
            !readIntChecked(buf, size, off, &s->typeLength[i]) ||
            dt < DT_INT || dt > DT_BOOL || s->typeLength[i] < 0)
            goto invalid;
        s->dataTypes[i] = (DataType)dt;
    }
    for (int i = 0; i < s->keySize; i++) {
        if (!readIntChecked(buf, size, off, &s->keyAttrs[i]) ||
            s->keyAttrs[i] < 0 || s->keyAttrs[i] >= s->numAttr)
            goto invalid;
    }
    if (!readIntChecked(buf, size, off, &e->hasIndex) ||
        (e->hasIndex != 0 && e->hasIndex != 1)) goto invalid;
    if (!readStringChecked(buf, size, off, &e->indexName)) goto invalid;
    if (!e->hasIndex && e->indexName[0] != '\0') goto invalid;
    if (e->indexName[0] == '\0') { free(e->indexName); e->indexName = NULL; }

    e->next = NULL;
    *result = e;
    return RC_OK;

allocation:
    if (e->schema) {
        if (e->schema->attrNames) {
            for (int i = 0; i < e->schema->numAttr; i++) free(e->schema->attrNames[i]);
        }
        free(e->schema->attrNames);
        free(e->schema->dataTypes);
        free(e->schema->typeLength);
        free(e->schema->keyAttrs);
        free(e->schema);
    }
    free(e->tableName);
    free(e);
    return RC_ALLOCATION_FAILED;
invalid:
    if (e->schema) {
        if (e->schema->attrNames) {
            for (int i = 0; i < e->schema->numAttr; i++) free(e->schema->attrNames[i]);
        }
        free(e->schema->attrNames);
        free(e->schema->dataTypes);
        free(e->schema->typeLength);
        free(e->schema->keyAttrs);
        free(e->schema);
    }
    free(e->tableName);
    free(e->indexName);
    free(e);
    return RC_RM_INVALID_SCHEMA_DATA;
}

/* ================================================================== */
/*  Persistence: flush the in-memory list to catalog.bin             */
/* ================================================================== */

static RC
catalogFlush(void)
{
    /* compute total size */
    int total = sizeof(int) * 3; /* magic, version, count */
    CatalogEntry *e = g_catalog.head;
    while (e) {
        total += entrySize(e);
        e = e->next;
    }
    int numPages = (total - 1) / PAGE_SIZE + 1;
    if (numPages < 1) numPages = 1;

    char *buf = (char *) calloc(numPages, PAGE_SIZE);
    int off = 0;
    int magic = CATALOG_MAGIC, version = CATALOG_VERSION;
    memcpy(buf + off, &magic, sizeof(int)); off += sizeof(int);
    memcpy(buf + off, &version, sizeof(int)); off += sizeof(int);
    memcpy(buf + off, &g_catalog.count, sizeof(int)); off += sizeof(int);
    e = g_catalog.head;
    while (e) {
        entrySerialize(buf, &off, e);
        e = e->next;
    }

    /* ensure file has enough pages */
    RC rc = ensureCapacity(numPages, &g_catFH);
    if (rc != RC_OK) { free(buf); return rc; }
    g_catFH.curPagePos = 0;
    for (int i = 0; i < numPages; i++) {
        rc = writeBlock(i, &g_catFH, buf + i * PAGE_SIZE);
        if (rc != RC_OK) { free(buf); return rc; }
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
        return catalogFlush();      /* write empty validated header */
    }

    /* load entries from disk */
    int totalBytes = g_catFH.totalNumPages * PAGE_SIZE;
    if (totalBytes < (int)(sizeof(int) * 3)) {
        closePageFile(&g_catFH);
        free(g_catalog.pageFile);
        return RC_RM_INVALID_SCHEMA_DATA;
    }
    char *buf = (char *) malloc((size_t)totalBytes);
    if (buf == NULL) return RC_ALLOCATION_FAILED;
    for (int i = 0; i < g_catFH.totalNumPages; i++) {
        RC rc = readBlock(i, &g_catFH, buf + i * PAGE_SIZE);
        if (rc != RC_OK) { free(buf); return rc; }
    }

    int off = 0;
    int magic, version;
    if (!readIntChecked(buf, totalBytes, &off, &magic) ||
        !readIntChecked(buf, totalBytes, &off, &version) ||
        !readIntChecked(buf, totalBytes, &off, &g_catalog.count) ||
        magic != CATALOG_MAGIC || version != CATALOG_VERSION ||
        g_catalog.count < 0 || g_catalog.count > CATALOG_MAX_ENTRIES) {
        free(buf);
        closePageFile(&g_catFH);
        free(g_catalog.pageFile);
        return RC_RM_INVALID_SCHEMA_DATA;
    }

    CatalogEntry *tail = NULL;
    for (int i = 0; i < g_catalog.count; i++) {
        CatalogEntry *e = NULL;
        RC rc = entryDeserialize(buf, totalBytes, &off, &e);
        if (rc != RC_OK) {
            free(buf);
            while (g_catalog.head) {
                CatalogEntry *next = g_catalog.head->next;
                free(g_catalog.head->tableName);
                freeSchema(g_catalog.head->schema);
                free(g_catalog.head->indexName);
                free(g_catalog.head);
                g_catalog.head = next;
            }
            closePageFile(&g_catFH);
            free(g_catalog.pageFile);
            return rc;
        }
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
    if (!g_initialized) {
        RC rc = initCatalog();
        if (rc != RC_OK) return rc;
    }

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
    RC rc = catalogFlush();
    if (rc != RC_OK) {
        /* Roll back the in-memory append if persistence failed. */
        CatalogEntry *prev = NULL, *cur = g_catalog.head;
        while (cur && cur != e) { prev = cur; cur = cur->next; }
        if (prev) prev->next = NULL; else g_catalog.head = NULL;
        g_catalog.count--;
        free(e->tableName);
        freeSchema(e->schema);
        free(e->indexName);
        free(e);
    }
    return rc;
}

RC
catalogDropTable(const char *tableName)
{
    if (!g_initialized) {
        RC rc = initCatalog();
        if (rc != RC_OK) return rc;
    }

    CatalogEntry *prev = NULL, *cur = g_catalog.head;
    while (cur) {
        if (strcmp(cur->tableName, tableName) == 0) {
            if (prev) prev->next = cur->next;
            else g_catalog.head = cur->next;
            g_catalog.count--;
            RC rc = catalogFlush();
            if (rc != RC_OK) {
                if (prev) prev->next = cur; else g_catalog.head = cur;
                g_catalog.count++;
                return rc;
            }
            free(cur->tableName);
            if (cur->schema) freeSchema(cur->schema);
            if (cur->indexName) free(cur->indexName);
            free(cur);
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
