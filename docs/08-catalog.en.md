# Chapter 8 · System Catalog

The catalog is an in-memory registry of table names, schemas, and primary-key index metadata. The DML executor consults it before opening a table and index. `openTable` still restores its schema from the table file, keeping this teaching design simple rather than making the catalog the sole source of truth.

`catalog.bin` starts with a magic value (`SDBC`), a format version, and an entry count, followed by serialized entries that may cross page boundaries.

Loading validates the header, count limits, attribute counts, string lengths, file bounds, data types, and key-column ranges. Invalid data returns `RC_RM_INVALID_SCHEMA_DATA` instead of driving unchecked allocations or copies.

Registration deep-copies schemas and rolls back its in-memory append if persistence fails. Drop restores its list entry if persistence fails. DDL initializes the catalog automatically and propagates failures. These are process-level compensations, not crash-atomic transactions; there is no WAL.

Verify with `make`, `./build/test_ddl`, and `./build/test_dml`.
