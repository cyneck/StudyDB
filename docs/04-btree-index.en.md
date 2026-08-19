# Chapter 4 · Teaching B+ Tree Index

Page 0 stores key type, order, root page, node count, and entry count. Other pages hold internal or leaf nodes; linked leaves support ordered scans.

The implementation supports lifecycle operations, point lookup, insertion and splitting, ordered scans, duplicate rejection, and leaf deletion.

Deletion is deliberately simplified: it shifts entries inside a leaf but does not borrow, merge, update parent separators, or shrink the root. It is therefore a teaching B+ tree with basic delete behavior, not a complete production deletion algorithm. It also has no concurrency, page checksums, WAL, or crash recovery.

Run `make` and `./build/test_btree` to verify it.
