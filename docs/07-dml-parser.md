# 第7章 · DML 解析器 DML Parser

> 对应源文件：`dml_parser.c` / `dml_parser.h`
>
> DDL 让我们能**建表**了，但建完表想**看一眼数据**还得写 C 代码调 `insertRecord`/`getRecord`——这对数据库用户来说太底层。DML 解析器把 `SELECT`/`INSERT`/`UPDATE`/`DELETE` 这四条最常用的 SQL 语句变成可解析的语法树，是「让数据库像数据库」的关键一步。

---

## 7.1 为什么需要这一层

**中文**

第5章的 DDL 解析器只解决了一半问题：它能创建/删除表，但无法对表里的数据做任何操作。在真实的数据库里，**99% 的用户操作是 DML**（Data Manipulation Language）——查询、插入、更新、删除记录。

如果没有 DML 解析器，学生要插入一条记录得这样写：

```c
Record *r = createRecord(schema);
setAttr(r, schema, 0, &intVal(1));
setAttr(r, schema, 1, &stringVal("alice"));
insertRecord(tableHandle, r);   // 还得先 openTable、createTable...
```

有了 DML 解析器，同样的事一句话就能做：

```sql
INSERT INTO users VALUES (1, 'alice', 25);
```

本章的 DML 解析器只负责**把字符串解析成结构化的 `DML_Statement`**，不直接执行——执行交给下一章的查询执行器。这种「解析与执行分离」的设计是所有现代数据库的标准做法，好处是：①解析器可以单独测试；②同一个解析结果可以喂给不同的执行策略（顺序扫描、索引扫描）。

**English**

The DDL parser from Chapter 5 only solves half the problem: it can create and drop tables, but it cannot touch the rows inside them. In a real database, **99% of user traffic is DML** — SELECT/INSERT/UPDATE/DELETE.

Without a DML parser, inserting one row looks like this:

```c
Record *r = createRecord(schema);
setAttr(r, schema, 0, &intVal(1));
setAttr(r, schema, 1, &stringVal("alice"));
insertRecord(tableHandle, r);   // plus openTable, createTable, ...
```

With a DML parser, the same thing is one line:

```sql
INSERT INTO users VALUES (1, 'alice', 25);
```

The parser in this chapter only **turns a string into a `DML_Statement` tree** — it does not execute anything. Execution is the job of the next chapter's query executor. This "parse vs. execute" split is standard in every real database, because: ① the parser can be tested in isolation; ② the same parsed tree can later feed different execution strategies (sequential scan, index scan, etc.).

---

## 7.2 核心原理：BNF 文法

**中文**

DML 解析器采用**递归下降（recursive descent）**：每个非终结符对应一个 C 函数，函数之间互相调用，刚好反映文法的递归结构。支持的文法（关键字大小写不敏感，分号结尾）：

```
<stmt>    ::= <select> | <insert> | <update> | <delete>
<select>  ::= SELECT <collist> FROM <name> [WHERE <expr>] ';'
<collist> ::= '*' | <col> (',' <col>)*
<insert>  ::= INSERT INTO <name> ['(' <col_list> ')']
              VALUES '(' <vallist> ')' ';'
<vallist> ::= <val> (',' <val>)*
<update>  ::= UPDATE <name> SET <assign> (',' <assign>)* [WHERE <expr>] ';'
<assign>  ::= <col> '=' <val>
<delete>  ::= DELETE FROM <name> [WHERE <expr>] ';'
<expr>    ::= <term> (OR <term>)*
<term>    ::= <factor> (AND <factor>)*
<factor>  ::= [NOT] <cmp> | '(' <expr> ')'
<cmp>     ::= <col> ('='|'<'|'>') <val>
```

WHERE 子句的优先级链是 **OR < AND < NOT < 比较**——和 SQL 标准一致。`<expr>` 之所以要写成递归的 `<term> (OR <term>)*` 而不是「先扫描所有 OR 再扫描所有 AND」，是为了让优先级**直接编码在调用栈里**：越外层的函数优先级越低，越内层越高。这是递归下降最优雅的地方。

**English**

The DML parser is **recursive descent**: every non-terminal maps to one C function, and the call graph mirrors the grammar's recursion. The supported grammar is:

```
<stmt>    ::= <select> | <insert> | <update> | <delete>
<select>  ::= SELECT <collist> FROM <name> [WHERE <expr>] ';'
<collist> ::= '*' | <col> (',' <col>)*
<insert>  ::= INSERT INTO <name> ['(' <col_list> ')']
              VALUES '(' <vallist> ')' ';'
<vallist> ::= <val> (',' <val>)*
<update>  ::= UPDATE <name> SET <assign> (',' <assign>)* [WHERE <expr>] ';'
<assign>  ::= <col> '=' <val>
<delete>  ::= DELETE FROM <name> [WHERE <expr>] ';'
<expr>    ::= <term> (OR <term>)*
<term>    ::= <factor> (AND <factor>)*
<factor>  ::= [NOT] <cmp> | '(' <expr> ')'
<cmp>     ::= <col> ('='|'<'|'>') <val>
```

WHERE precedence is **OR < AND < NOT < comparison** — same as SQL. The reason `<expr>` is written as `<term> (OR <term>)*` instead of "scan all ORs then all ANDs" is to **encode precedence directly in the call stack**: outer functions are low precedence, inner functions are high precedence. That is the elegant part of recursive descent.

---

## 7.3 关键数据结构

**中文**

`DML_Statement` 用一个 union-ish 的结构同时表示四种语句——只填跟当前类型相关的字段，其余字段置零：

```c
typedef struct DML_Statement {
    DML_StmtType type;       // SELECT / INSERT / UPDATE / DELETE
    char *tableName;          // 所有语句都涉及一张表

    /* SELECT */
    char **columns;           // 显式列名列表，NULL 表示 SELECT *
    int    numCols;
    int    selectAll;         // 1 if SELECT *

    /* WHERE (SELECT/UPDATE/DELETE 共用) */
    Expr   *where;            // NULL 表示没有 WHERE
    char  **whereColNames;    // 见 7.4.3：列引用占位机制
    int     numWhereCols;

    /* INSERT */
    Value **values;
    int     numValues;

    /* UPDATE */
    char  **setCols;
    Value **setVals;
    int     numSets;
} DML_Statement;
```

`Expr` 树来自 `expr.h`，由 `MAKE_BINOP_EXPR` / `MAKE_UNOP_EXPR` / `MAKE_ATTRREF` / `MAKE_CONS` 四个宏构造，节点类型只有三种：`EXPR_OP`（运算符）、`EXPR_CONST`（常量）、`EXPR_ATTRREF`（列引用）。复用现成的 Expr 树意味着解析器一建好树，下一章的 `evalExpr(record, schema, expr, &result)` 就能直接拿来用。

**English**

`DML_Statement` is a union-ish struct that holds all four statement types — only fields relevant to the current `type` are filled in, the rest are zeroed:

```c
typedef struct DML_Statement {
    DML_StmtType type;
    char *tableName;

    /* SELECT */
    char **columns;           // explicit column list; NULL means SELECT *
    int    numCols;
    int    selectAll;

    /* WHERE (shared by SELECT/UPDATE/DELETE) */
    Expr   *where;            // NULL if no WHERE clause
    char  **whereColNames;    // see 7.4.3: column-ref placeholder
    int     numWhereCols;

    /* INSERT */
    Value **values;
    int     numValues;

    /* UPDATE */
    char  **setCols;
    Value **setVals;
    int     numSets;
} DML_Statement;
```

The `Expr` tree comes from `expr.h`, built by four macros: `MAKE_BINOP_EXPR` / `MAKE_UNOP_EXPR` / `MAKE_ATTRREF` / `MAKE_CONS`. There are only three node types: `EXPR_OP`, `EXPR_CONST`, `EXPR_ATTRREF`. Reusing the existing Expr tree means that once the parser builds it, the next chapter's `evalExpr(record, schema, expr, &result)` can consume it directly.

---

## 7.4 关键代码逐行讲

### 7.4.1 Tokenizer：复用 DDL 的 Lexer 框架

**中文**

DML 的 tokenizer 与 `ddl_parser.c` 的几乎一字不差——同一个 `TokKind`/`Token`/`Lexer` 布局，同一组 `lexInit`/`lexNext`/`lexIs`/`lexExpect` 函数，连 `skipWsAndComments` 里 `--` 行注释的处理都一样。**复用**是这里的关键设计：DDL 已经把递归下降的脚手架写对了，没必要重写一遍。

DML 只是在 DDL 的基础上**加了几个 token 类型**：

```c
typedef enum {
    TOK_END, TOK_IDENT, TOK_INT, TOK_FLOAT, TOK_STRING,
    TOK_LPAREN, TOK_RPAREN, TOK_COMMA, TOK_SEMI, TOK_STAR,
    TOK_EQ, TOK_LT, TOK_GT, TOK_UNKNOWN
} TokKind;
```

相比 DDL 的 `TokKind` 多出了：`TOK_FLOAT`（浮点字面量）、`TOK_STRING`（单引号字符串，支持 `''` 转义）、`TOK_STAR`（`SELECT *` 的星号）、`TOK_EQ`/`TOK_LT`/`TOK_GT`（三个比较运算符）。所有标识符在词法层就被 `toupper` 转成大写，所以 `SELECT`/`select`/`Select` 一视同仁。

`lexNext` 里字符串字面量的解析值得一看——它处理了 SQL 的 `''` 转义（两个连续单引号表示一个字面单引号）：

```c
if (c == '\'') {
    lx->src++;                       /* skip opening quote */
    size_t cap = 16, len = 0;
    char *buf = (char *) malloc(cap);
    while (*lx->src) {
        if (*lx->src == '\'') {
            if (lx->src[1] == '\'') {        /* escaped quote '' */
                /* append one quote, skip two */
                buf[len++] = '\'';
                lx->src += 2;
                continue;
            }
            break;                           /* closing quote */
        }
        buf[len++] = *lx->src++;
    }
    if (*lx->src == '\'') lx->src++;         /* skip closing quote */
    buf[len] = '\0';
    lx->cur.kind = TOK_STRING;
    lx->cur.text = buf;
    return RC_OK;
}
```

**English**

The DML tokenizer is almost a verbatim copy of the one in `ddl_parser.c` — same `TokKind`/`Token`/`Lexer` layout, same `lexInit`/`lexNext`/`lexIs`/`lexExpect` helpers, even `--` line comments are skipped the same way. **Reuse** is the design point: DDL already got the recursive-descent scaffolding right, so there's no reason to rewrite it.

DML only **adds a few token kinds** on top:

```c
typedef enum {
    TOK_END, TOK_IDENT, TOK_INT, TOK_FLOAT, TOK_STRING,
    TOK_LPAREN, TOK_RPAREN, TOK_COMMA, TOK_SEMI, TOK_STAR,
    TOK_EQ, TOK_LT, TOK_GT, TOK_UNKNOWN
} TokKind;
```

Compared to DDL it adds: `TOK_FLOAT`, `TOK_STRING` (single-quoted, with `''` escape), `TOK_STAR` (for `SELECT *`), and `TOK_EQ`/`TOK_LT`/`TOK_GT`. All identifiers are upper-cased at lex time, so `SELECT`/`select`/`Select` are identical.

The string-literal branch of `lexNext` is worth a look — it handles the SQL `''` escape (two consecutive single quotes = one literal quote):

```c
if (c == '\'') {
    lx->src++;                       /* skip opening quote */
    size_t cap = 16, len = 0;
    char *buf = (char *) malloc(cap);
    while (*lx->src) {
        if (*lx->src == '\'') {
            if (lx->src[1] == '\'') {        /* escaped quote '' */
                buf[len++] = '\'';
                lx->src += 2;
                continue;
            }
            break;                           /* closing quote */
        }
        buf[len++] = *lx->src++;
    }
    if (*lx->src == '\'') lx->src++;         /* skip closing quote */
    buf[len] = '\0';
    lx->cur.kind = TOK_STRING;
    lx->cur.text = buf;
    return RC_OK;
}
```

---

### 7.4.2 WHERE 表达式：递归下降

**中文**

WHERE 子句的递归下降总共就三个函数，对应三层优先级：

```c
/* <expr>  ::= <term> (OR <term>)*        — 最低优先级 */
static RC parseOrExpr (Lexer *lx, StrVec *colNames, Expr **out);

/* <term>  ::= <factor> (AND <factor>)*   — 中等优先级 */
static RC parseAndExpr(Lexer *lx, StrVec *colNames, Expr **out);

/* <factor>::= [NOT] <cmp> | '(' <expr> ')'
                                          — 最高优先级 */
static RC parseFactor (Lexer *lx, StrVec *colNames, Expr **out);
```

调用链是 `parseOrExpr → parseAndExpr → parseFactor`，每个函数都把"剩下的"递归到下一层。`parseFactor` 里圆括号的处理是亮点——它**前向声明** `parseOrExpr` 然后递归调用，这样 `(a = 1 OR b = 2) AND c = 3` 也能正确解析：

```c
static RC
parseFactor(Lexer *lx, StrVec *colNames, Expr **out)
{
    if (lexIs(lx, "NOT")) {                /* NOT 前缀 */
        lexNext(lx);
        Expr *child = NULL;
        RC rc = parseFactor(lx, colNames, &child);
        if (rc != RC_OK) return rc;
        MAKE_UNOP_EXPR(*out, child, OP_BOOL_NOT);
        return RC_OK;
    }
    if (lx->cur.kind == TOK_LPAREN) {       /* 括号 → 整个 OR 表达式 */
        lexNext(lx);
        Expr *inner = NULL;
        RC rc = parseOrExpr(lx, colNames, &inner);
        if (rc != RC_OK) return rc;
        if (lx->cur.kind != TOK_RPAREN) { /* 必须有匹配的 ')' */
            fprintf(stderr, "[dml] expected ')' in expression\n");
            freeExpr(inner);
            return RC_RM_INVALID_SCHEMA_DATA;
        }
        lexNext(lx);
        *out = inner;
        return RC_OK;
    }
    return parseCmp(lx, colNames, out);     /* 否则就是一个比较谓词 */
}
```

注意 `NOT` 后面跟的又是 `parseFactor`，所以 `NOT NOT a = 1` 也合法（双重否定）。`parseAndExpr`/`parseOrExpr` 都是「先解析一个，再循环吃 AND/OR」的相同套路：

```c
static RC
parseAndExpr(Lexer *lx, StrVec *colNames, Expr **out)
{
    RC rc = parseFactor(lx, colNames, out);    /* 第一个 factor */
    if (rc != RC_OK) return rc;

    while (lexIs(lx, "AND")) {                 /* 后续 0 或多个 */
        lexNext(lx);
        Expr *rhs = NULL;
        rc = parseFactor(lx, colNames, &rhs);
        if (rc != RC_OK) { freeExpr(*out); *out = NULL; return rc; }
        Expr *combined;
        MAKE_BINOP_EXPR(combined, *out, rhs, OP_BOOL_AND);
        *out = combined;                       /* 左结合 */
    }
    return RC_OK;
}
```

每次循环把当前树作为左子树、新解析的作为右子树，自然实现**左结合**。`parseOrExpr` 只是把 `AND` 换成 `OR`、`OP_BOOL_AND` 换成 `OP_BOOL_OR`，结构完全相同。

**English**

The WHERE clause has three functions, one per precedence level:

```c
/* <expr>  ::= <term> (OR <term>)*        — lowest precedence */
static RC parseOrExpr (Lexer *lx, StrVec *colNames, Expr **out);

/* <term>  ::= <factor> (AND <factor>)*   — middle precedence */
static RC parseAndExpr(Lexer *lx, StrVec *colNames, Expr **out);

/* <factor>::= [NOT] <cmp> | '(' <expr> ')'
                                          — highest precedence */
static RC parseFactor (Lexer *lx, StrVec *colNames, Expr **out);
```

The call chain is `parseOrExpr → parseAndExpr → parseFactor`, each one punting "the rest" to the next level. The highlight of `parseFactor` is how it handles parentheses — it **forward-declares** `parseOrExpr` and recurses into it, so `(a = 1 OR b = 2) AND c = 3` parses correctly:

```c
static RC
parseFactor(Lexer *lx, StrVec *colNames, Expr **out)
{
    if (lexIs(lx, "NOT")) {                /* NOT prefix */
        lexNext(lx);
        Expr *child = NULL;
        RC rc = parseFactor(lx, colNames, &child);
        if (rc != RC_OK) return rc;
        MAKE_UNOP_EXPR(*out, child, OP_BOOL_NOT);
        return RC_OK;
    }
    if (lx->cur.kind == TOK_LPAREN) {       /* '(' → full OR-expr */
        lexNext(lx);
        Expr *inner = NULL;
        RC rc = parseOrExpr(lx, colNames, &inner);
        if (rc != RC_OK) return rc;
        if (lx->cur.kind != TOK_RPAREN) { /* need matching ')' */
            fprintf(stderr, "[dml] expected ')' in expression\n");
            freeExpr(inner);
            return RC_RM_INVALID_SCHEMA_DATA;
        }
        lexNext(lx);
        *out = inner;
        return RC_OK;
    }
    return parseCmp(lx, colNames, out);     /* otherwise a comparison */
}
```

Note that `NOT` is followed by another `parseFactor`, so `NOT NOT a = 1` is legal (double negation). `parseAndExpr`/`parseOrExpr` both follow the same pattern — "parse one, then loop on AND/OR":

```c
static RC
parseAndExpr(Lexer *lx, StrVec *colNames, Expr **out)
{
    RC rc = parseFactor(lx, colNames, out);    /* first factor */
    if (rc != RC_OK) return rc;

    while (lexIs(lx, "AND")) {                 /* zero or more */
        lexNext(lx);
        Expr *rhs = NULL;
        rc = parseFactor(lx, colNames, &rhs);
        if (rc != RC_OK) { freeExpr(*out); *out = NULL; return rc; }
        Expr *combined;
        MAKE_BINOP_EXPR(combined, *out, rhs, OP_BOOL_AND);
        *out = combined;                       /* left-associative */
    }
    return RC_OK;
}
```

Each loop iteration makes the current tree the left child and the newly parsed factor the right child, naturally giving **left-associativity**. `parseOrExpr` is the same code with `AND`→`OR` and `OP_BOOL_AND`→`OP_BOOL_OR`.

---

### 7.4.3 列引用占位机制 + `>` 转 `<`

**中文**

这里有两个相互关联的精巧设计。

**问题一**：解析 WHERE 时，解析器**还不知道表的结构**——`users.age` 到底是第几列？要等执行器查过 catalog 才知道。

**解决**：先用 `attrRef = -1` 占位，把列名单独存到 `whereColNames` 数组里。执行器之后做中序遍历 Expr 树，每遇到一个 `-1` 的 ATTRREF，就从 `whereColNames` 取下一个名字去 catalog 查实际索引并填进去。

**问题二**：`expr.h` 里只定义了两种比较运算符：

```c
typedef enum OpType {
    OP_BOOL_AND, OP_BOOL_OR, OP_BOOL_NOT,
    OP_COMP_EQUAL,        // =
    OP_COMP_SMALLER       // <
} OpType;
```

没有 `OP_COMP_GREATER`。那 `age > 18` 怎么办？

**解决**：交换操作数。`a > b` 等价于 `b < a`，所以遇到 `TOK_GT` 时把 `cons` 放左边、`attrRef` 放右边：

```c
static RC
parseCmp(Lexer *lx, StrVec *colNames, Expr **out)
{
    RC rc;
    char *colName = NULL;
    CHECKEX(rc = parseColumnName(lx, &colName));
    rc = svPush(colNames, colName);          /* 记下列名 */
    free(colName);
    if (rc != RC_OK) return rc;

    OpType op;
    int    swap = 0;
    switch (lx->cur.kind) {
        case TOK_EQ: op = OP_COMP_EQUAL;   swap = 0; break;
        case TOK_LT: op = OP_COMP_SMALLER; swap = 0; break;
        case TOK_GT: op = OP_COMP_SMALLER; swap = 1; break;  /* ← 关键 */
        default:
            fprintf(stderr, "[dml] expected =, <, or >\n");
            return RC_RM_INVALID_SCHEMA_DATA;
    }
    lexNext(lx);

    Value *val = NULL;
    CHECKEX(rc = parseValue(lx, &val));

    Expr *attrRef;
    MAKE_ATTRREF(attrRef, -1);          /* 占位：执行器填实际索引 */

    Expr *cons;
    MAKE_CONS(cons, val);

    if (swap) {
        /* col > val  ==>  val < col */
        MAKE_BINOP_EXPR(*out, cons, attrRef, op);
    } else {
        MAKE_BINOP_EXPR(*out, attrRef, cons, op);
    }
    return RC_OK;
}
```

`swap` 标志位记录是否要交换操作数——`a > b` 变成 `b < a`，仍然用 `OP_COMP_SMALLER` 求值，结果完全一样。这种「**用现有限算符 + 操作数换位**」的技巧避免了改 `expr.h` 的公共接口，是工程上很实用的妥协。

`whereColNames` 是按**中序遍历顺序**追加的（每解析到一个 `<col>` 就 `svPush` 一次），所以执行器只要按相同顺序遍历 Expr 树、依次取 `whereColNames[i]` 就能对上号。

**English**

Here are two interrelated design subtleties.

**Problem 1**: when parsing WHERE, the parser **does not yet know the table schema** — what column index is `users.age`? That information only comes from the catalog at execution time.

**Solution**: use `attrRef = -1` as a placeholder, and store the column name separately in the `whereColNames` array. The executor later does an in-order traversal of the Expr tree; every time it meets an ATTRREF with `-1`, it pops the next name from `whereColNames`, looks it up in the catalog, and fills in the real index.

**Problem 2**: `expr.h` only defines two comparison operators:

```c
typedef enum OpType {
    OP_BOOL_AND, OP_BOOL_OR, OP_BOOL_NOT,
    OP_COMP_EQUAL,        // =
    OP_COMP_SMALLER       // <
} OpType;
```

There is no `OP_COMP_GREATER`. So what about `age > 18`?

**Solution**: swap the operands. `a > b` is equivalent to `b < a`, so when we see `TOK_GT`, we put `cons` on the left and `attrRef` on the right:

```c
static RC
parseCmp(Lexer *lx, StrVec *colNames, Expr **out)
{
    RC rc;
    char *colName = NULL;
    CHECKEX(rc = parseColumnName(lx, &colName));
    rc = svPush(colNames, colName);          /* record name */
    free(colName);
    if (rc != RC_OK) return rc;

    OpType op;
    int    swap = 0;
    switch (lx->cur.kind) {
        case TOK_EQ: op = OP_COMP_EQUAL;   swap = 0; break;
        case TOK_LT: op = OP_COMP_SMALLER; swap = 0; break;
        case TOK_GT: op = OP_COMP_SMALLER; swap = 1; break;  /* ← key */
        default:
            fprintf(stderr, "[dml] expected =, <, or >\n");
            return RC_RM_INVALID_SCHEMA_DATA;
    }
    lexNext(lx);

    Value *val = NULL;
    CHECKEX(rc = parseValue(lx, &val));

    Expr *attrRef;
    MAKE_ATTRREF(attrRef, -1);          /* placeholder: executor fills in */

    Expr *cons;
    MAKE_CONS(cons, val);

    if (swap) {
        /* col > val  ==>  val < col */
        MAKE_BINOP_EXPR(*out, cons, attrRef, op);
    } else {
        MAKE_BINOP_EXPR(*out, attrRef, cons, op);
    }
    return RC_OK;
}
```

The `swap` flag records whether operands need swapping — `a > b` becomes `b < a` and still evaluates with `OP_COMP_SMALLER`, with identical results. This trick of "**use existing operators + swap operands**" avoids touching the public `expr.h` interface — a very practical engineering compromise.

`whereColNames` is appended in **in-order traversal order** (every time a `<col>` is parsed, we `svPush` once), so the executor only needs to traverse the Expr tree the same way and pull `whereColNames[i]` in order to match.

---

## 7.5 完整 API 一览

| 函数 | 作用 | 一句话 |
|------|------|--------|
| `parseDML(sql, &out)` | 解析 SQL 字符串 | 返回堆分配的 `DML_Statement*` |
| `freeDMLStatement(stmt)` | 释放语句 | 递归 free 所有内部分配 |

支持的语句类型：

| `DML_StmtType` | 关键字 | 关键字段 |
|----------------|--------|----------|
| `DML_SELECT` | `SELECT` | `columns`/`selectAll`, `where` |
| `DML_INSERT` | `INSERT INTO` | `columns`(可选), `values` |
| `DML_UPDATE` | `UPDATE ... SET` | `setCols`/`setVals`, `where` |
| `DML_DELETE` | `DELETE FROM` | `where` |

以上是解析器能识别的语句类型，不代表完整 SQL 语义。当前执行器即使收到
`SELECT col1, col2` 也会打印完整记录；SELECT 不会规划 B+ 树索引扫描，而是
执行记录扫描。INSERT 的可选列名列表可以被解析，但执行阶段仍要求按 schema
顺序为所有字段提供一个值。主键列不允许 UPDATE。WHERE 只支持字面量、
`=`/`<`/`>` 与 `AND`/`OR`/`NOT`；尚无 JOIN、NULL、排序、分组、事务，以及
表和索引写入失败时的原子回滚。

---

## 7.6 编译与验证

```bash
# 编译所有目标（包含 test_dml）
make all

# 运行 DML 测试套件
./build/test_dml
```

`test_dml.c` 包含 6 个测试：

1. **testParseSelect** — 解析 `SELECT *` 和 `SELECT col, col`，检查 `tableName` 大写化、`selectAll` 标志、列数。
2. **testParseInsert** — 解析 `INSERT INTO users VALUES (1, 'alice', 25)`，检查 3 个 `Value` 的类型和值（`DT_INT`/`DT_STRING`）。
3. **testParseUpdateDelete** — 解析 `UPDATE ... SET col = val WHERE ...` 和 `DELETE FROM ... WHERE ...`，检查 `setCols`/`setVals` 配对、`where` 非空。
4. **testExecuteInsertSelect** — 端到端：`CREATE TABLE` → 3 次 `INSERT` → `SELECT *` 看到全部 3 行；并验证「3 个值插 2 列表」会被执行器拒绝。
5. **testWhereFilter** — 插 5 行后跑 `SELECT * FROM T2 WHERE val < 30`，期望返回 2 行（val=10, 20）。
6. **testCatalogIntegration** — 验证 `CREATE TABLE` 后 catalog 里有对应条目、`DROP TABLE` 后消失。

测试 4-6 实际跨了解析器/执行器/catalog 三层——这正是「解析器不执行、只产 AST」设计的直接受益：解析器建好树后由 `executeSQL` 接力完成动作。

预期输出末尾：

```
ALL DML TESTS PASSED
```

---

## 7.7 思考题

1. **为什么 WHERE 表达式复用 `expr.h` 的 Expr 树，而不是为 DML 单独定义一套条件节点？** 提示：考虑 `evalExpr` 已有的实现、AST 复用、与第3章 record manager 测试链路的协同。

2. **`a > b` 通过交换操作数转成 `b < a`。这个技巧在哪些情况下会失效？** 提示：考虑非对称语义——比如 SQL 的 `NULL` 比较三值逻辑、字符串排序规则、用户自定义类型。如果想严格支持 `>`，应该改 `expr.h` 的哪个部分？

3. **列引用占位用 `attrRef = -1`，列名另存到 `whereColNames`。如果改成「解析阶段就查 catalog 把 attrRef 填好」，会对解析器的设计带来什么影响？** 提示：解析器与 catalog 的耦合、单元测试的隔离性、同一条 SQL 在不同 schema 下的可重用性。

---

> **下一章**：[第8章 · 系统目录](08-catalog.md)
