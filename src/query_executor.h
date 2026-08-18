/**
 * @file query_executor.h
 * @authors Xingli Li
 * @date 2023-07-29
 * @brief Execute parsed DML statements against the record/index managers.
 *
 * The executor is the bridge between the parser (which produces a
 * DML_Statement AST) and the storage engine (record_mgr + btree_mgr).
 * It resolves column names via the catalog, then runs the appropriate
 * storage-engine operations.
 *
 * Flow:
 *   SQL string → parseDML → DML_Statement → executeDML → result
 */
#ifndef QUERY_EXECUTOR_H
#define QUERY_EXECUTOR_H

#include "dberror.h"
#include "dml_parser.h"

/**
 * @brief Execute a parsed DML statement.
 *
 * SELECT  : scans the table, prints matching records to stdout.
 * INSERT  : creates a record, inserts it, updates index if present.
 * UPDATE  : scans matching records, applies SET assignments.
 * DELETE  : scans matching records, deletes them + index entries.
 *
 * @return RC_OK on success, error code on failure.
 */
extern RC executeDML(DML_Statement *stmt);

/**
 * @brief Parse and execute a DML string end-to-end.
 * Convenience wrapper: parseDML + executeDML + freeDMLStatement.
 */
extern RC executeSQL(const char *sql);

#endif /* QUERY_EXECUTOR_H */
