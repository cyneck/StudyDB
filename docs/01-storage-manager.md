# 第1章 · 存储管理器

> 对应源码：`src/storage_mgr.c`、`src/storage_mgr.h`

## 1.1 Page file 模型

StudyDB 把一个文件视为固定大小页面的数组，页面大小由 `PAGE_SIZE` 定义为4096字节。`SM_FileHandle` 保存文件名、总页数、当前位置和内部 `FILE *`。

## 1.2 核心操作

- `createPageFile`：创建含一个零页的新文件；
- `openPageFile`：验证文件长度是页面大小的整数倍；
- `readBlock`：只允许读取 `0 <= pageNum < totalNumPages` 的完整页面；
- `writeBlock`：覆盖已有页，或写入紧邻文件末尾的新页；
- `appendEmptyBlock`：追加一个零页；
- `ensureCapacity(n)`：循环追加，直到文件至少有 n 页。

所有 I/O 都检查 `fseek`、`fread`、`fwrite` 和 `fflush` 的结果。读取不存在的页面不会伪装成成功。

## 1.3 当前边界

本层使用 C 标准库文件接口。`fflush` 不等于磁盘级 `fsync`，项目也没有 WAL，因此只适合教学，不提供事务持久性保证。

## 1.4 验证

```bash
make
./build/test_storage_buffer
```

思考：为什么 `writeBlock` 不允许直接写到远离文件末尾的第100页，而由 `ensureCapacity` 负责补齐中间页面？
