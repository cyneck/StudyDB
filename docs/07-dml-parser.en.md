# Chapter 7 · DML Parser

> Corresponding source files: `dml_parser.c` / `dml_parser.h`
>
> DDL lets us **create tables**, but to **peek at the data** after creating them we still have to write C code that calls `insertRecord`/`getRecord` — far too low-level for database users. The DML parser turns the four most common SQL statements (`SELECT`/`INSERT`/`UPDATE`/`DELETE`) into a parseable syntax tree, the key step that "makes a database feel like a database."

---

## 7.1 Why we need this layer

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

## 7.2 Core principle: BNF grammar

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

## 7.3 Key data structures

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

## 7.4 Line-by-line walkthrough

### 7.4.1 Tokenizer: reusing the DDL lexer framework

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

### 7.4.2 WHERE expression: recursive descent

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

### 7.4.3 Column reference placeholder + `>` to `<`

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

## 7.5 Complete API reference

| Function | Role | In one sentence |
|----------|------|-----------------|
| `parseDML(sql, &out)` | Parse a SQL string | Returns a heap-allocated `DML_Statement*` |
| `freeDMLStatement(stmt)` | Free the statement | Recursively frees all internal allocations |

Supported statement types:

| `DML_StmtType` | Keyword | Key fields |
|----------------|---------|------------|
| `DML_SELECT` | `SELECT` | `columns`/`selectAll`, `where` |
| `DML_INSERT` | `INSERT INTO` | `columns` (optional), `values` |
| `DML_UPDATE` | `UPDATE ... SET` | `setCols`/`setVals`, `where` |
| `DML_DELETE` | `DELETE FROM` | `where` |

These are parser capabilities, not full SQL semantics. The current executor
always prints complete records even for `SELECT col1, col2`; it does not use the
B+ tree to plan `SELECT` and performs a record scan. An optional INSERT column
list is parsed but execution still requires exactly one value per schema
attribute in schema order. Updating a primary-key column is rejected. WHERE is
limited to literals, `=`/`<`/`>`, `AND`/`OR`/`NOT`; there are no joins, NULLs,
ordering, grouping, transactions, or atomic rollback across table/index writes.

---

## 7.6 Build and verify

```bash
# 编译所有目标（包含 test_dml）
make all

# 运行 DML 测试套件
./build/test_dml
```

`test_dml.c` contains six tests:

1. **testParseSelect** — parses `SELECT *` and `SELECT col, col`, checks `tableName` upper-casing, the `selectAll` flag, and column counts.
2. **testParseInsert** — parses `INSERT INTO users VALUES (1, 'alice', 25)`, checks the three `Value`s' types and values (`DT_INT`/`DT_STRING`).
3. **testParseUpdateDelete** — parses `UPDATE ... SET col = val WHERE ...` and `DELETE FROM ... WHERE ...`, checks `setCols`/`setVals` pairing and that `where` is non-null.
4. **testExecuteInsertSelect** — end-to-end: `CREATE TABLE` → 3 `INSERT`s → `SELECT *` sees all 3 rows; also verifies that "3 values into a 2-column table" is rejected by the executor.
5. **testWhereFilter** — after inserting 5 rows, runs `SELECT * FROM T2 WHERE val < 30`, expecting 2 rows back (val=10, 20).
6. **testCatalogIntegration** — verifies that after `CREATE TABLE` the catalog has the entry, and after `DROP TABLE` it's gone.

Tests 4–6 actually cross all three layers — parser/executor/catalog. That's the direct benefit of the "parser doesn't execute, just produces AST" design: once the parser builds the tree, `executeSQL` takes over to finish the work.

Expected output tail:

```
ALL DML TESTS PASSED
```

---

## 7.7 Exercises

1. **Why does the WHERE expression reuse the Expr tree from `expr.h` instead of defining a separate set of condition nodes for DML?** Hint: think about the existing `evalExpr` implementation, AST reuse, and the synergy with the Chapter 3 record manager test pipeline.

2. **`a > b` is converted to `b < a` by swapping operands. In what cases does this trick break?** Hint: think about asymmetric semantics — e.g. SQL `NULL` comparison three-valued logic, string collation rules, user-defined types. If you wanted to strictly support `>`, which part of `expr.h` should change?

3. **Column-reference placeholders use `attrRef = -1`, with column names stored separately in `whereColNames`. If you instead looked up the catalog at parse time to fill in `attrRef`, what impact would that have on the parser's design?** Hint: coupling between parser and catalog, unit-test isolation, reusability of the same SQL under different schemas.

---

> **Next chapter**: [Chapter 8 · Catalog](08-catalog.en.md)
