# Chapter 5 · DDL Parser

The parser supports `CREATE TABLE` and `DROP TABLE`, with INT, FLOAT, BOOL, `STRING(n)`, and a single-column primary key. Identifiers are normalized to uppercase.

CREATE initializes the catalog, creates the table, optionally creates its B+ tree, and registers metadata. Later failures compensate by deleting earlier files. DROP verifies the catalog entry and propagates file or catalog errors.

There is no transaction manager, so this is explicit error handling and best-effort compensation rather than crash-atomic DDL.

Run `make` and `./build/test_ddl`.
