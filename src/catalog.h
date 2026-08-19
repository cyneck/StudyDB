/**
 * @file catalog.h
 * @authors Xingli Li
 * @date 2023-07-29
 * @brief System catalog: a registry of all tables and their schemas.
 *
 * The catalog is the database's "table of tables". It maps table names to
 * their schemas and index metadata, so that `SELECT * FROM users` can find
 * out what columns `users` has and whether it has an index.
 *
 * Persistence: the catalog is stored in its own page file ("catalog.bin").
 * The file starts with a magic value, format version, and entry count;
 * serialised entries follow and may span page boundaries.
 * On init the file is loaded into an in-memory linked list; on shutdown
 * (or register/drop) the list is flushed back to disk.
 *
 * Memory model:
 *   Catalog -> in-memory linked list of CatalogEntry
 *     .tableName   heap string
 *     .schema      rebuilt from the serialised blob
 *     .hasIndex    0 or 1
 *     .indexName   heap string (or NULL if no index)
 */
#ifndef CATALOG_H
#define CATALOG_H

#include "dberror.h"
#include "tables.h"

/** One table entry in the catalog. */
typedef struct CatalogEntry {
    char  *tableName;
    Schema *schema;
    int    hasIndex;       /* 0 = no index, 1 = has a B+ tree index */
    char  *indexName;      /* e.g. "users.idx", or NULL */
    struct CatalogEntry *next;
} CatalogEntry;

/** The catalog handle. */
typedef struct Catalog {
    CatalogEntry *head;
    int           count;
    char          *pageFile;   /* "catalog.bin" */
} Catalog;

/* lifecycle */
extern RC initCatalog(void);
extern RC shutdownCatalog(void);

/* table registration */
extern RC catalogRegisterTable(const char *tableName, Schema *schema,
                               int hasIndex, const char *indexName);
extern RC catalogDropTable(const char *tableName);

/* lookup */
extern CatalogEntry *catalogLookupTable(const char *tableName);

/* list all table names (caller frees every name and the returned array) */
extern RC catalogListTables(char ***names, int *count);

/* for debugging */
extern void catalogPrint(void);

#endif /* CATALOG_H */
