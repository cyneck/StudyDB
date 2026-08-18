# 第6章 · 端到端集成实践 Integration

> 对应源文件：`demo_api.c`
>
> 前面五章各自独立——存储、缓冲池、记录管理、B+ 树索引、DDL 解析。本章把它们**串成一条完整的调用链**，演示一个真实数据库引擎的工作流：建表 → 插数据 → 索引查找 → 范围扫描。

> **说明（2026-08）**：本章所有代码只调用三层管理器的**公开 API**、不依赖内部结构，因此第 2/3 章换用新实现（链表缓冲池、标记式记录页）后，本章内容依然成立，无需改动。

---

## 6.1 为什么需要这一章

**中文**

单看每一层都好理解，但学生最常见的困惑是：「这些层怎么协同工作？我写一条 `CREATE TABLE` 到底触发了哪些动作？索引到底是和表一起维护的，还是独立的？」

`demo_api.c` 就是回答这些问题的「最小可运行样例」。它不引入任何新机制，只把前面 5 章的 API 按真实顺序调一遍：先用 DDL 建表+建索引文件，再打开表和索引句柄，然后插记录时**同时**写表和写索引，最后用索引做点查和范围扫描。读懂这个 demo，就理解了关系数据库存储引擎的骨架。

**English**

Each layer is simple in isolation, but the most common student question is: *how do these layers cooperate? What exactly does a `CREATE TABLE` trigger? Is the index maintained together with the table, or separately?*

`demo_api.c` is the minimal runnable sample that answers these. It introduces no new mechanisms — it simply calls the APIs from the previous five chapters in the order a real engine would: DDL creates the table and index files, we open both handles, every insert writes to the table **and** the index, and finally we use the index for a point lookup and a range scan. Read this demo and you understand the skeleton of a relational storage engine.

---

## 6.2 demo_api.c 流程总览

**中文**

`demo_api.c` 的 `main` 一共 7 步，对应数据库引擎一次完整的「建表 → 用表 → 拆表」生命周期：

```
┌──────────────────────────────────────────────────────────────────┐
│  1. initRecordManager / initIndexManager    ← 初始化子系统        │
│  2. executeDDL("CREATE TABLE users ...")     ← DDL 建表 + 建索引  │
│  3. openTable + openBTree                    ← 拿到两个句柄        │
│  4. insertUser × 5                           ← 插记录 + 维护索引   │
│  5. findKey + getRecord                      ← 索引点查           │
│  6. openTreeScan + nextEntry 循环            ← 索引范围扫描       │
│  7. closeBTree / closeTable / DROP TABLE     ← 清理              │
└──────────────────────────────────────────────────────────────────┘
```

关键点：**表和索引是两个独立的存储对象**（`USERS` 表文件 + `USERS.idx` 索引文件），由调用方负责保持一致——`insertUser` 每次先 `insertRecord` 再 `insertKey`，把记录的 RID 作为索引项的值存进 B+ 树。

**English**

`demo_api.c`'s `main` has 7 steps — a full "create → use → drop" lifecycle:

```
┌──────────────────────────────────────────────────────────────────┐
│  1. initRecordManager / initIndexManager    ← init subsystems     │
│  2. executeDDL("CREATE TABLE users ...")     ← DDL creates table  │
│  3. openTable + openBTree                    ← get two handles    │
│  4. insertUser × 5                           ← insert + maintain  │
│  5. findKey + getRecord                      ← index point lookup │
│  6. openTreeScan + nextEntry loop            ← index range scan   │
│  7. closeBTree / closeTable / DROP TABLE     ← cleanup            │
└──────────────────────────────────────────────────────────────────┘
```

Key insight: **the table and the index are two independent storage objects** (`USERS` table file + `USERS.idx` index file). The caller is responsible for keeping them consistent — `insertUser` first calls `insertRecord`, then `insertKey`, storing the record's RID as the value side of the B+ tree entry.

---

## 6.3 关键代码逐段讲

### 6.3.1 `insertUser`：双写辅助函数

**中文**

这是整个 demo 的核心 helper：构造一条记录，先写表，再把 `(key → RID)` 写进索引：

```c
static RC insertUser(RM_TableData *tbl, BTreeHandle *idx,
                     int id, const char *name, int age)
{
    Schema *sch = tbl->schema;
    Record *r;
    createRecord(&r, sch);

    Value *v;
    MAKE_VALUE(v, DT_INT, id);      setAttr(r, sch, 0, v);  freeVal(v);
    MAKE_STRING_VALUE(v, name);     setAttr(r, sch, 1, v);  freeVal(v);
    MAKE_VALUE(v, DT_INT, age);     setAttr(r, sch, 2, v);  freeVal(v);

    /* 1. insert into the table (assigns r->id = RID) */
    insertRecord(tbl, r);

    /* 2. insert (id -> RID) into the primary-key index */
    Value *key; MAKE_VALUE(key, DT_INT, id);
    insertKey(idx, key, r->id);
    freeVal(key);

    freeRecord(r);
    return RC_OK;
}
```

注意顺序不能反：必须先 `insertRecord`，因为它会分配 RID（`r->id`，即 `page.slot`），索引项的值正是这个 RID。如果先建索引再插记录，就拿不到正确的 RID。

**English**

This is the core helper of the demo: build a record, write it to the table, then write `(key → RID)` into the index:

```c
static RC insertUser(RM_TableData *tbl, BTreeHandle *idx,
                     int id, const char *name, int age)
{
    Schema *sch = tbl->schema;
    Record *r;
    createRecord(&r, sch);

    Value *v;
    MAKE_VALUE(v, DT_INT, id);      setAttr(r, sch, 0, v);  freeVal(v);
    MAKE_STRING_VALUE(v, name);     setAttr(r, sch, 1, v);  freeVal(v);
    MAKE_VALUE(v, DT_INT, age);     setAttr(r, sch, 2, v);  freeVal(v);

    /* 1. insert into the table (assigns r->id = RID) */
    insertRecord(tbl, r);

    /* 2. insert (id -> RID) into the primary-key index */
    Value *key; MAKE_VALUE(key, DT_INT, id);
    insertKey(idx, key, r->id);
    freeVal(key);

    freeRecord(r);
    return RC_OK;
}
```

The order matters: `insertRecord` must come first because it assigns the RID (`r->id` = `page.slot`), and that RID is the value stored in the index entry. Reverse the order and you'd have no RID to put in the tree.

---

### 6.3.2 点查：`findKey` + `getRecord`

**中文**

「查 id=20 的用户」分两步：先在 B+ 树里把 key 翻译成 RID，再用 RID 到表里取记录：

```c
Value *q; MAKE_VALUE(q, DT_INT, 20);
RID rid;
if (findKey(idx, q, &rid) == RC_OK) {
    Record *r;
    createRecord(&r, tbl.schema);
    getRecord(&tbl, rid, r);
    char *s = serializeRecord(r, tbl.schema);
    printf("   found at [%d-%d]: %s\n", rid.page, rid.slot, s);
    free(s);
    freeRecord(r);
}
freeVal(q);
```

`findKey` 走 B+ 树从根到叶的查找路径，复杂度 O(log n)；`getRecord` 用 `rid.page` 找到那一页（经缓冲池），用 `rid.slot` 在页内定位记录。这就是关系数据库里「主键等值查询」的标准实现。

**English**

"Find user with id=20" is two steps: translate the key to a RID via the B+ tree, then fetch the record by RID:

```c
Value *q; MAKE_VALUE(q, DT_INT, 20);
RID rid;
if (findKey(idx, q, &rid) == RC_OK) {
    Record *r;
    createRecord(&r, tbl.schema);
    getRecord(&tbl, rid, r);
    char *s = serializeRecord(r, tbl.schema);
    printf("   found at [%d-%d]: %s\n", rid.page, rid.slot, s);
    free(s);
    freeRecord(r);
}
freeVal(q);
```

`findKey` walks the B+ tree root-to-leaf in O(log n); `getRecord` uses `rid.page` to find the page (via the buffer pool) and `rid.slot` to locate the record within it. This is the textbook implementation of a primary-key equality query.

---

### 6.3.3 范围扫描：`openTreeScan` + `nextEntry`

**中文**

「按 id 升序列出所有用户」用索引扫描：

```c
BT_ScanHandle *sc = NULL;
openTreeScan(idx, &sc);
RID cur;
Record *r;
createRecord(&r, tbl.schema);
while (nextEntry(sc, &cur) == RC_OK) {
    getRecord(&tbl, cur, r);
    char *s = serializeRecord(r, tbl.schema);
    printf("   %s\n", s);
    free(s);
}
freeRecord(r);
closeTreeScan(&sc);
```

`openTreeScan` 把游标定位到最左叶子节点的第一条记录；每次 `nextEntry` 沿着**叶子层的兄弟指针**向右走，按 key 升序吐出 RID。这就是 B+ 树为什么叶子要串成链表——为了支持范围扫描而无需回到上层节点。

**English**

"List all users ordered by id" uses an index scan:

```c
BT_ScanHandle *sc = NULL;
openTreeScan(idx, &sc);
RID cur;
Record *r;
createRecord(&r, tbl.schema);
while (nextEntry(sc, &cur) == RC_OK) {
    getRecord(&tbl, cur, r);
    char *s = serializeRecord(r, tbl.schema);
    printf("   %s\n", s);
    free(s);
}
freeRecord(r);
closeTreeScan(&sc);
```

`openTreeScan` positions the cursor at the first entry of the leftmost leaf; each `nextEntry` follows the **leaf-level sibling pointer** to the right, emitting RIDs in ascending key order. This is exactly why B+ tree leaves are linked into a list — to support range scans without ever climbing back to internal nodes.

---

## 6.4 索引 vs 线性扫描

**中文**

这是本章最关键的对比。同一张表，「按 id 升序列出所有用户」有两种实现：

- **线性扫描**：用 `RM_ScanHandle` 从第 0 页第 0 槽开始，逐页逐槽遍历，把每条记录的 id 收集起来再排序。复杂度 O(n) 读记录 + O(n log n) 排序。
- **索引扫描**：用 `BT_ScanHandle` 沿叶子链表走。叶子节点已经按 key 有序，且每页能装很多 key，所以只需扫叶子节点本身，复杂度 O(叶子数) ≈ O(n / 叶子扇出)。

| 维度 | 线性扫描 `startScan` | 索引扫描 `openTreeScan` |
|------|----------------------|-------------------------|
| 数据来源 | 表文件每一页每一槽 | B+ 树叶子节点 |
| 顺序保证 | 无序，需外部排序 | 天然升序 |
| 读放大 | 每条记录都读一次 | 每个 key 走链表指针 |
| 复杂度 | O(n) I/O | O(叶子数) ≈ O(n / F) I/O |
| 适用场景 | 全表统计、无索引列 | 主键/索引列上的范围或排序 |

为什么索引扫描更快？关键在于 **B+ 树的叶子节点之间有 `next` 指针**（见 `btree_mgr.c:14`）。范围扫描不需要回到内层节点重新导航，直接顺着叶子链表「向右走」即可；而线性扫描要遍历表的所有页（包括空闲槽、被删除的槽），且拿到的数据无序，需要额外排序。

**English**

This is the most important comparison in the chapter. For "list all users ordered by id" there are two implementations:

- **Linear scan**: use `RM_ScanHandle` to walk page 0 slot 0 onward, page by page, slot by slot, collect every record's id, then sort. Cost: O(n) record reads + O(n log n) sort.
- **Index scan**: use `BT_ScanHandle` to walk the leaf chain. Leaves are already sorted by key and pack many keys per page, so you only touch leaves: O(#leaves) ≈ O(n / leaf fanout).

| Dimension | Linear scan `startScan` | Index scan `openTreeScan` |
|-----------|-------------------------|---------------------------|
| Source | every page/slot of the table file | B+ tree leaf nodes |
| Order | unordered, needs external sort | naturally ascending |
| Read amplification | reads every record | follows leaf-chain pointers |
| Complexity | O(n) I/O | O(#leaves) ≈ O(n / F) I/O |
| Use case | full-table stats, non-indexed columns | range/sort on indexed column |

Why is the index scan faster? The key is the **`next` pointer between B+ tree leaves** (see `btree_mgr.c:14`). A range scan never climbs back to internal nodes — it just walks right along the leaf chain. A linear scan, by contrast, must traverse every page of the table (including empty/deleted slots) and then sort the unordered result.

---

## 6.5 预期输出

**中文**

运行 `./build/demo_api`，预期输出如下：

```
== 1. executeDDL: CREATE TABLE users ==
== 2. open table + index ==
== 3. insert 5 users ==
   table tuples: 5,  index entries: 5
== 4. index lookup: WHERE id = 20 ==
   found at [1-1]: (20,dave,28)
== 5. index scan: all users in id order ==
   (10,bob,30)
   (20,dave,28)
   (30,alice,25)
   (40,eve,35)
   (50,carol,22)
== 6. cleanup ==
== done ==
```

注意插入顺序是 **30, 10, 50, 20, 40**（乱序），但第 5 步扫描结果严格按 **10, 20, 30, 40, 50** 升序返回。这正是 B+ 树索引的核心能力：**插入时维护排序，查询时免费获得有序结果**。`RID` 显示 `[1-1]` 说明 id=20 这条记录落在表的第 1 页第 1 槽——页号和槽号由记录管理器分配，与 key 无关。

**English**

Running `./build/demo_api` produces:

```
== 1. executeDDL: CREATE TABLE users ==
== 2. open table + index ==
== 3. insert 5 users ==
   table tuples: 5,  index entries: 5
== 4. index lookup: WHERE id = 20 ==
   found at [1-1]: (20,dave,28)
== 5. index scan: all users in id order ==
   (10,bob,30)
   (20,dave,28)
   (30,alice,25)
   (40,eve,35)
   (50,carol,22)
== 6. cleanup ==
== done ==
```

Note the insertion order is **30, 10, 50, 20, 40** (out of order), but step 5 returns them strictly as **10, 20, 30, 40, 50**. This is the core promise of a B+ tree: **sorting is maintained at insert time; ordered output is free at query time.** The RID `[1-1]` means record id=20 lives at page 1, slot 1 of the table file — page/slot are assigned by the record manager and are unrelated to the key.

---

## 6.6 整体架构回顾：调用链

**中文**

把 demo_api.c 的每个 API 调用一路追到磁盘 I/O，得到这张调用链图：

```
demo_api.c 的调用                  经过层                       最终落到
─────────────────────────────────────────────────────────────────────
executeDDL("CREATE TABLE..")  → ddl_parser  → record_mgr   → buffer_mgr → storage_mgr → 磁盘建文件
openTable / openBTree         → record_mgr / btree_mgr     → buffer_mgr → storage_mgr → 读首页
insertRecord                  → record_mgr                 → buffer_mgr → storage_mgr → 写页
insertKey                     → btree_mgr                  → buffer_mgr → storage_mgr → 写页
findKey                       → btree_mgr                  → buffer_mgr → storage_mgr → 读页
getRecord                     → record_mgr                 → buffer_mgr → storage_mgr → 读页
openTreeScan / nextEntry      → btree_mgr (走叶子链表)      → buffer_mgr → storage_mgr → 读页
```

主数据路径保持清晰边界：`record_mgr` 不直接 `fread`，而是通过 `buffer_mgr`；缓冲未命中时才调用 `storage_mgr.readBlock`。但这不是严格的单链分层：`btree_mgr` 与 `record_mgr` 并列依赖页服务，DDL/DML 执行器会组合多个模块。只有保持公开接口和语义兼容时，实现替换才能对上层透明。

**English**

Tracing each demo_api.c API call all the way down to disk I/O:

```
demo_api.c call                    layers traversed              hits disk
─────────────────────────────────────────────────────────────────────
executeDDL("CREATE TABLE..")  → ddl_parser → record_mgr    → buffer_mgr → storage_mgr → file create
openTable / openBTree         → record_mgr / btree_mgr     → buffer_mgr → storage_mgr → read header page
insertRecord                  → record_mgr                 → buffer_mgr → storage_mgr → page write
insertKey                     → btree_mgr                  → buffer_mgr → storage_mgr → page write
findKey                       → btree_mgr                  → buffer_mgr → storage_mgr → page read
getRecord                     → record_mgr                 → buffer_mgr → storage_mgr → page read
openTreeScan / nextEntry      → btree_mgr (leaf chain)     → buffer_mgr → storage_mgr → page read
```

The main data path has a clear boundary: `record_mgr` does not call `fread` directly; it goes through `buffer_mgr`, which calls `storage_mgr.readBlock` on a miss. This is not a strict linear stack: `btree_mgr` and `record_mgr` are sibling consumers of page services, and the DDL/DML executor composes several modules. Replacement is transparent only while public interfaces and semantics remain compatible.

---

## 6.7 已知限制与扩展方向

**中文**

`demo_api.c`（以及它背后的引擎）刻意做了简化，留出扩展空间：

1. **低层 C API 不自动维护索引**：`demo_api.c` 调用 `insertRecord` 后必须显式调用 `insertKey`。SQL 路径的 `query_executor` 会维护主键索引，但绕过执行器直接调用记录 API 仍可能造成不一致。真实系统通常在统一的执行/存储事务路径中原子维护表和索引。
2. **`deleteKey` 不重平衡**：删除只从叶子摘除 key，不做借位（borrow）或合并（merge），见 `btree_mgr.c:11` 的注释。树仍然正确，但可能下溢（under-full）。这是 CS525 课程的常见简化。
3. **本示例本身不走 DML**：`demo_api.c` 直接调用 C API；项目级 DML 支持由第7章的 `dml_parser`、`query_executor` 和 `demo_sql.c` 提供。
4. **单线程、单连接**：没有事务、没有锁、没有并发控制。
5. **没有查询优化器**：是走索引还是线性扫描，目前由调用方决定。

扩展方向：①让表与索引更新具备原子性；②实现 `deleteKey` 的借位/合并；③在现有 DML 子集上增加 JOIN、ORDER BY 等语法；④加一个基于代价的优化器，自动选择索引扫描或线性扫描。

**English**

`demo_api.c` (and the engine behind it) deliberately simplifies things, leaving room for extension:

1. **The low-level C API does not maintain indexes automatically**: after `insertRecord`, `demo_api.c` must call `insertKey`. The SQL path's `query_executor` maintains the primary-key index, but direct record-API use can still create divergence. Production systems update table and indexes atomically in a unified execution/storage transaction path.
2. **`deleteKey` does not rebalance**: deletion only removes the key from its leaf — no borrow, no merge (see the comment at `btree_mgr.c:11`). The tree stays correct but may become under-full. This is the standard CS525 simplification.
3. **This demo does not use DML**: `demo_api.c` calls the C API directly. Project-level DML support is provided by Chapter 7's `dml_parser`, `query_executor`, and `demo_sql.c`.
4. **Single-threaded, single-connection**: no transactions, no locks, no concurrency control.
5. **No query optimizer**: index scan vs. linear scan is currently chosen by the caller.

Extensions to consider: ① make table/index updates atomic; ② implement borrow/merge in `deleteKey`; ③ extend the limited DML grammar with joins and ordering; ④ add a cost-based optimizer that picks between index scan and linear scan automatically.

---

## 6.8 思考题

1. **为什么 `insertUser` 必须先 `insertRecord` 再 `insertKey`，而不能反过来？** 如果反过来，索引里存的是什么 RID？提示：`r->id` 是什么时候被赋值的。

2. **假设 `users` 表有 100 万条记录，每页装 100 条，B+ 树叶子扇出 F=200。做一次 `SELECT * FROM users ORDER BY id`，线性扫描和索引扫描各需要大约多少次页 I/O？** 提示：线性扫描要读所有数据页并排序；索引扫描只走叶子链表。

3. **当前 `deleteKey` 不做重平衡。如果在一个有 100 万 key 的 B+ 树上连续删除 90 万条，查询性能会怎么退化？树的高度会变吗？** 提示：下溢不改变树高，但叶子节点利用率下降。

---

> **完整代码**：见 [demo_api.c](../demo_api.c)。返回 [README](../README.md) 看项目总览。
