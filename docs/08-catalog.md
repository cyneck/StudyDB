# 第8章 · 系统目录 Catalog

> 对应源码：`src/catalog.c`、`src/catalog.h`

## 8.1 作用

Catalog 是内存中的表注册表，记录表名、Schema、是否有主键索引及索引文件名。DML executor 先查询 Catalog，再打开相应表和索引。

`openTable` 仍直接从表文件恢复 Schema，因此当前 Catalog 是 SQL 层的集中元数据，而不是唯一的 Schema 真相来源。这是为了保持模块简单。

## 8.2 文件格式

Catalog 保存在独立的 `catalog.bin` page file。文件开头是：

```text
magic (SDBC) | format version | entry count | serialized entries ...
```

每个条目包含表名、属性数量、主键数量、各属性名称/类型/长度、主键列、索引标志和索引文件名。条目可以跨页连续存放。

加载时会检查：

- magic 和格式版本；
- 文件至少包含完整 header；
- entry count、属性数量和字符串长度上限；
- 每次读取都没有越过文件末尾；
- 数据类型和主键列范围合法。

格式不合法返回 `RC_RM_INVALID_SCHEMA_DATA`，而不是继续用损坏的长度进行分配或复制。

## 8.3 生命周期和一致性

- `initCatalog`：不存在时创建空目录，存在时验证并加载；
- `catalogRegisterTable`：深复制 Schema，写盘失败时回滚内存链表；
- `catalogDropTable`：从链表暂时移除，写盘失败时恢复；
- `shutdownCatalog`：flush 后释放内存。

DDL 会自动初始化 Catalog，并传播注册/删除失败。由于项目没有事务和 WAL，这些是进程内补偿操作，不能保证断电时的原子性。

## 8.4 验证

```bash
make
./build/test_ddl
./build/test_dml
```

思考：若要在线兼容旧格式，应怎样设计 version migration，而不是直接拒绝旧文件？
