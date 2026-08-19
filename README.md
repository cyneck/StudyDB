# 简易数据库引擎 · A Simple Database Engine

> **中文版** ｜ [English version](README.en.md)
>
> IIT CS525 高级数据库系统课程实践项目 —— 从零实现一个分层数据库存储引擎。
>
> A hands-on project for IIT CS525 (Advanced Database Systems): build a layered
> database storage engine from scratch, one layer at a time.

本项目把数据库引擎拆成若干可独立理解和测试的模块。主数据路径是「磁盘 → 缓冲池 → 记录管理」；B+ 树索引与记录管理并列使用底层页服务，DDL/DML 执行器再组合记录、索引和 Catalog。因此它不是严格的单向五层依赖。

---

## 架构总览 / Architecture

```
┌─────────────────────────────────────────────┐
│  第5层  DDL 解析器    ddl_parser.c          │  ← CREATE/DROP TABLE
├──────────────────┬──────────────────────────┤
│ 第3层 记录管理器  │  第4层 B+树索引          │  ← 表 CRUD / 索引查找
│  record_mgr.c    │   btree_mgr.c            │
├──────────────────┴──────────────────────────┤
│  第2层  缓冲池管理器  buffer_mgr.c          │  ← FIFO/LRU（链表+时间戳）
├─────────────────────────────────────────────┤
│  第1层  存储管理器    storage_mgr.c         │  ← page file I/O
├─────────────────────────────────────────────┤
│           磁盘  Disk (page files)            │
└─────────────────────────────────────────────┘
```

每一层「为什么存在」「怎么实现」「怎么测试」都在对应章节里讲清楚。

---

## 循序渐进阅读路线 / Reading Path

| 章节 | 主题 | 源文件 | 核心问题 |
|------|------|--------|----------|
| [第1章](docs/01-storage-manager.md) | 存储管理器 | `storage_mgr.c` | 磁盘 I/O 的最小单位是什么？ |
| [第2章](docs/02-buffer-manager.md) | 缓冲池管理器 | `buffer_mgr.c` | 为什么不能每次都读写磁盘？ |
| [第3章](docs/03-record-manager.md) | 记录管理器 | `record_mgr.c` | 怎么在 page 之上组织「表」？ |
| [第4章](docs/04-btree-index.md) | B+树索引 | `btree_mgr.c` | 怎么把查找从 O(n) 降到 O(log n)？ |
| [第5章](docs/05-ddl-parser.md) | DDL 解析器 | `ddl_parser.c` | 怎么让用户用 SQL 建表？ |
| [第6章](docs/06-integration.md) | 集成实践 | `demo_api.c` | 串起来跑：索引 vs 线性扫描 |
| [第7章](docs/07-dml-parser.md) | DML 解析器 | `dml_parser.c` | 怎么让用户写 SELECT/INSERT？ |
| [第8章](docs/08-catalog.md) | 系统目录 | `catalog.c` | 数据库怎么知道有哪些表？ |
| [第9章](docs/09-postgresql-overview.md) | PostgreSQL 全景 | — | 真实数据库还长什么样？ |

**每章结构**：为什么 → 原理 → 数据结构 → 关键代码 → 编译运行 → 思考题

---

## 快速开始 / Quick Start

```bash
# 编译所有目标
make clean && make all

# 运行全部自动化测试
make test

# 跑原有回归测试（9 用例，含 10000 条插入）
./build/test_assign3_1

# 跑 B+ 树单元测试（6 用例）
./build/test_btree

# 跑 DDL 解析单元测试（5 用例）
./build/test_ddl

# 跑 DML + Catalog 单元测试（含重复主键一致性检查）
./build/test_dml

# 跑 C API 端到端示例：DDL 建表 → 插记录 → 索引查找 → 范围扫描
./build/demo_api

# 跑 SQL 端到端示例：用 SQL 语句完成 INSERT/SELECT/UPDATE/DELETE 全流程
./build/demo_sql
```

---

## 文件清单 / File Map

所有源码和测试文件均在 `src/` 目录下。按章节顺序排列，同一章的文件连续放在一起。

### 核心源码（按章节顺序）

| 文件 | 角色 | 章节 |
|------|------|------|
| `src/storage_mgr.c/h` | page file 读写 | 第1章 |
| `src/buffer_mgr.c/h` | 缓冲池 + FIFO/LRU 替换策略（链表+时间戳） | 第2章 |
| `src/buffer_mgr_stat.c/h` | 缓冲池统计信息 | 第2章 |
| `src/record_mgr.c/h` | 表 CRUD + 线性扫描 | 第3章 |
| `src/record_mgr_ex.h` | 记录管理器内部结构（遗留，已不参与构建） | 第3章 |
| `src/expr.c/h` | 表达式求值（WHERE 条件树） | 第3章 |
| `src/rm_serializer.c` | 记录/Schema/Value 序列化打印 | 第3章 |
| `src/tables.h` | 核心数据类型（Value/RID/Record/Schema/RM_TableData） | 第3章 |
| `src/btree_mgr.c/h` | B+ 树索引（分裂/查找/范围扫描） | 第4章 |
| `src/ddl_parser.c/h` | DDL 解析器（CREATE/DROP TABLE） | 第5章 |
| `src/demo_api.c` | C API 端到端示例（DDL→插入→索引查找→扫描） | 第6章 |
| `src/dml_parser.c/h` | DML 解析器（SELECT/INSERT/UPDATE/DELETE） | 第7章 |
| `src/query_executor.c/h` | DML 执行器（解析列名→驱动 record_mgr+btree_mgr） | 第7章 |
| `src/demo_sql.c` | SQL 端到端示例（用 SQL 语句完成全流程） | 第7章 |
| `src/catalog.c/h` | 系统目录（表的注册表，持久化到 catalog.bin） | 第8章 |

### 测试文件

| 文件 | 角色 | 关联章节 |
|------|------|----------|
| `src/test_assign3_1.c` | 课程原始回归测试（9 用例，含 10000 条插入） | 第3章 |
| `src/test_expr.c` | 表达式求值测试 | 第3章 |
| `src/test_storage_buffer.c` | 存储边界、稀疏扩容、pin/unpin 测试 | 第1-2章 |
| `src/test_btree.c` | B+ 树单元测试（6 用例） | 第4章 |
| `src/test_ddl.c` | DDL 解析单元测试（5 用例） | 第5章 |
| `src/test_dml.c` | DML + Catalog 与表/索引一致性测试 | 第7-8章 |

### 基础设施

| 文件 | 角色 |
|------|------|
| `src/dberror.c/h` | 返回码定义 + 错误消息（RC_OK/RC_IM_*/RC_RM_* 等） |
| `src/dt.h` | bool 类型定义 |
| `src/test_helper.h` | 测试宏（TEST_CHECK/ASSERT_TRUE/TEST_DONE） |
| `Makefile` | 构建脚本（`make all` 编译 src/ 下全部源码） |

---

## 技术栈 / Tech Stack

- **语言**：C99（`-std=c99 -g -Wall -Wextra -Werror`）
- **依赖**：仅 libc（`stdio/stdlib/string/math`），无第三方库
- **平台**：Linux / macOS / WSL / MinGW
- **构建**：GNU Make

---

## 许可 / License

本项目采用 [MIT License](LICENSE)，可用于学习、修改和分发。
