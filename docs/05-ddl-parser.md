# 第5章 · DDL 解析器

> 对应源码：`src/ddl_parser.c`、`src/catalog.c`

## 5.1 支持的语法

```sql
CREATE TABLE users (
  id INT,
  name STRING(32),
  active BOOL,
  PRIMARY KEY (id)
);

DROP TABLE users;
```

标识符统一转换为大写。支持 INT、FLOAT、BOOL 和 `STRING(n)`；当前只为单列主键创建一个 B+树索引。

## 5.2 CREATE 执行顺序

1. 初始化并检查 Catalog；
2. 创建表文件；
3. 有主键时创建索引文件；
4. 注册 Catalog。

后续步骤失败时会删除前面创建的文件，避免返回成功却留下未登记的表。Catalog 注册失败也会传播给调用者。

## 5.3 DROP 执行顺序

DROP 先确认 Catalog 中存在表，再删除表文件、索引文件和 Catalog 条目。项目没有事务管理器，因此它使用显式错误传播和补偿操作，而不提供崩溃时的原子 DDL 保证。

## 5.4 验证

```bash
make
./build/test_ddl
```
