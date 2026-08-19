# Chapter 1 · Storage Manager

StudyDB treats a file as an array of fixed 4096-byte pages. `SM_FileHandle` stores the name, page count, current position, and internal `FILE *`.

`createPageFile` creates one zero page. `readBlock` accepts only complete existing pages. `writeBlock` overwrites a page or appends exactly at the end. `ensureCapacity(n)` appends zero pages until the file contains at least n pages. Seek, read, write, and flush results are checked.

The layer uses C standard I/O. `fflush` is not `fsync`, and there is no WAL or transactional durability.

Verify with `make` and `./build/test_storage_buffer`.
