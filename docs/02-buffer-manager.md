# 第2章 · 缓冲池管理器

> 对应源码：`src/buffer_mgr.c`、`src/buffer_mgr.h`

## 2.1 为什么需要缓冲池

存储管理器每次访问都会操作文件。缓冲池在内存中保存固定数量的页副本，使上层能够复用热点页，并把多次修改合并为一次写回。

调用者必须遵守以下生命周期：

1. `pinPage`：取得页面并增加 `fixCount`；
2. 修改数据后调用 `markDirty`；
3. 使用结束后调用 `unpinPage`；
4. 只有 `fixCount == 0` 的页面可以被换出或由 `forceFlushPool` 刷盘。

重复 `unpinPage` 会返回错误，避免 `fixCount` 变成负数。

## 2.2 当前实现的数据结构

当前实现有意采用适合教学的小型链表结构，而不是哈希表：

```c
typedef struct BufferFrame {
    BM_PageHandle *pageHandle;
    bool dirty;
    int fixCount;
    int accessTimestamp;
    int entryTimestamp;
    struct BufferFrame *prev;
    struct BufferFrame *next;
} BufferFrame;
```

- 每个 frame 独立持有一个 `PAGE_SIZE` 页副本；
- 页面命中通过线性遍历查找，复杂度为 O(缓冲池大小)；
- `buffer_mgr.h` 只暴露公开句柄和 API，私有结构保留在 `.c` 中。

这种结构适合几十页规模的实验。若扩大缓冲池，可把线性查找替换为 `pageNum → frame` 哈希表，但这不是当前源码的一部分。

## 2.3 FIFO 与 LRU

当前只实现两种策略：

- FIFO：选择 `entryTimestamp` 最小且 `fixCount == 0` 的页面；
- LRU：选择 `accessTimestamp` 最小且 `fixCount == 0` 的页面。

`RS_CLOCK`、`RS_LFU` 和 `RS_LRU_K` 仅保留在公共枚举中，用于说明可扩展方向；初始化时选择这些策略会返回 `RC_BM_INVALID_STRATEGY`。

## 2.4 `pinPage` 流程

```text
检查参数 → 线性查找 → 命中则 fixCount++
                    → 未命中则选择空 frame 或未 pin 的 victim
                    → dirty victim 写回
                    → ensureCapacity(pageNum + 1)
                    → readBlock → fixCount = 1
```

`ensureCapacity(pageNum + 1)` 很重要：请求第10页时，文件会扩到11页，而不是只追加一个空页。

## 2.5 刷盘语义

- `forcePage`：强制写回指定页面；
- `forceFlushPool`：只写回 dirty 且未 pin 的页面；
- `shutdownBufferPool`：若仍有 pin 页面则拒绝关闭，否则 flush、关闭文件并释放内存。

这里的 `fflush` 只把 C 标准库缓冲交给操作系统，并不等价于事务数据库的持久化提交。项目没有 WAL、事务或崩溃恢复。

## 2.6 验证

```bash
make
./build/test_storage_buffer
```

边界测试覆盖：越界读失败、稀疏页扩容、dirty 页写回和重复 unpin。

## 2.7 思考题

1. 缓冲池扩大到10万页时，线性查找为什么会成为瓶颈？
2. 为什么不能把仍在修改的 pinned 页面标记为 clean？
3. 实现 LRU-K 需要为每个 frame 额外保存哪些历史信息？
