/**
 * @file ddl_parser.h
 * @authors Xingli Li
 * @date 2023-07-29
 * @brief Minimal DDL (Data Definition Language) parser.
 *
 * Supported grammar (case-insensitive keywords, ';' terminated):
 *
 *   <stmt>     ::= <create> | <drop>
 *   <create>   ::= CREATE TABLE <name> '(' <col_list> ')'
 *                  [PRIMARY KEY '(' <col> ')'] ';'
 *   <col_list> ::= <col> (',' <col>)*
 *   <col>      ::= <name> <type>
 *   <type>     ::= INT | FLOAT | BOOL | STRING '(' <len> ')'
 *   <drop>     ::= DROP TABLE <name> ';'
 *
 * The parser produces either a Schema (for CREATE) or a table name
 * (for DROP). A convenience executeDDL() runs the statement against the
 * existing record manager / index manager.
 */
#ifndef DDL_PARSER_H
#define DDL_PARSER_H

#include "dberror.h"
#include "tables.h"
#include "record_mgr.h"

/** Statement kind produced by the parser. */
typedef enum DDL_StmtType {
    DDL_CREATE_TABLE,
    DDL_DROP_TABLE,
    DDL_UNKNOWN
} DDL_StmtType;

/** A parsed DDL statement. */
typedef struct DDL_Statement {
    DDL_StmtType type;
    char *tableName;        /* heap-allocated, caller frees via freeDDLStatement */
    Schema *schema;         /* only for CREATE; NULL for DROP */
    int  primaryKeyAttr;    /* index of PK attr, or -1 if none declared */
} DDL_Statement;

/**
 * @brief Parse a single DDL statement string.
 * @param sql  NUL-terminated SQL text. May contain trailing ';' / whitespace.
 * @param out  Receives a heap-allocated DDL_Statement on success.
 * @return RC_OK on success, RC_RM_INVALID_SCHEMA_DATA on parse error.
 */
extern RC parseDDL(const char *sql, DDL_Statement **out);

/** Free a statement returned by parseDDL. */
extern RC freeDDLStatement(DDL_Statement *stmt);

/**
 * @brief Parse and execute a DDL statement end-to-end.
 *
 * CREATE TABLE -> createTable(name, schema); if a PRIMARY KEY is declared,
 *                 also creates an index file "<name>.idx" over the key.
 * DROP TABLE   -> deleteTable(name) + deletes the index file if present.
 *
 * @return RC_OK on success, an error code otherwise.
 */
extern RC executeDDL(const char *sql);

#endif /* DDL_PARSER_H */
