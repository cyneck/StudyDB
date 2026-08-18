/**
 * @file dml_parser.c
 * @authors Xingli Li
 * @date 2023-07-29
 * @brief Recursive-descent DML (SELECT/INSERT/UPDATE/DELETE) parser.
 *
 * The tokenizer is deliberately modeled after the one in ddl_parser.c
 * (same TokKind / Token / Lexer layout, same lexNext / lexIs / lexExpect
 * style) so the two parsers feel like one codebase.  It is extended to
 * recognise the extra tokens DML needs:
 *
 *   - single-quoted string literals    'foo'
 *   - floating-point literals          1.5
 *   - comparison operators             =  <  >
 *   - the SELECT * star                *
 *
 * Grammar (case-insensitive keywords, ';' terminated):
 *
 *   <stmt>    ::= <select> | <insert> | <update> | <delete>
 *   <select>  ::= SELECT <collist> FROM <name> [WHERE <expr>] ';'
 *   <collist> ::= '*' | <col> (',' <col>)*
 *   <insert>  ::= INSERT INTO <name> ['(' <col_list> ')']
 *                 VALUES '(' <vallist> ')' ';'
 *   <vallist> ::= <val> (',' <val>)*
 *   <update>  ::= UPDATE <name> SET <assign> (',' <assign>)* [WHERE <expr>] ';'
 *   <assign>  ::= <col> '=' <val>
 *   <delete>  ::= DELETE FROM <name> [WHERE <expr>] ';'
 *   <expr>    ::= <term> (OR <term>)*
 *   <term>    ::= <factor> (AND <factor>)*
 *   <factor>  ::= [NOT] <cmp> | '(' <expr> ')'
 *   <cmp>     ::= <col> ('='|'<'|'>') <val>
 *
 * WHERE expressions reuse the Expr tree from expr.h so that the existing
 * evalExpr() can evaluate them directly.  Because the parser does not yet
 * know the table schema, column references inside the Expr tree use
 * EXPR_ATTRREF with attrRef = -1 as a placeholder; the matching column
 * names are stored on the DML_Statement (whereColNames) in traversal
 * order so the executor can resolve them later.
 *
 * Memory policy: every successful parseDML() call returns a heap-allocated
 * DML_Statement whose strings/arrays the caller releases with
 * freeDMLStatement().
 */
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <ctype.h>

#include "dberror.h"
#include "tables.h"
#include "expr.h"
#include "dml_parser.h"

/* ================================================================== */
/*  Tokenizer                                                         */
/* ================================================================== */

typedef enum {
    TOK_END,
    TOK_IDENT,        /* identifier or keyword (upper-cased) */
    TOK_INT,          /* integer literal */
    TOK_FLOAT,        /* floating-point literal */
    TOK_STRING,       /* single-quoted string literal (text without quotes) */
    TOK_LPAREN,       /* ( */
    TOK_RPAREN,       /* ) */
    TOK_COMMA,        /* , */
    TOK_SEMI,         /* ; */
    TOK_STAR,         /* *  (used by SELECT *) */
    TOK_EQ,           /* = */
    TOK_LT,           /* < */
    TOK_GT,           /* > */
    TOK_UNKNOWN
} TokKind;

typedef struct {
    TokKind kind;
    char   *text;     /* heap-allocated; for TOK_IDENT upper-cased;
                         for TOK_STRING the raw content (no quotes) */
    int     intV;     /* for TOK_INT */
    double  floatV;   /* for TOK_FLOAT */
} Token;

typedef struct {
    const char *src;  /* current read cursor */
    Token       cur;  /* current token */
} Lexer;

static void
skipWsAndComments(Lexer *lx)
{
    for (;;) {
        while (*lx->src && isspace((unsigned char) *lx->src)) lx->src++;
        if (lx->src[0] == '-' && lx->src[1] == '-') {
            /* SQL line comment */
            lx->src += 2;
            while (*lx->src && *lx->src != '\n') lx->src++;
        } else {
            break;
        }
    }
}

/** Read the next token into lx->cur. Returns RC_OK on success. */
static RC
lexNext(Lexer *lx)
{
    skipWsAndComments(lx);
    free(lx->cur.text);
    lx->cur.text   = NULL;
    lx->cur.kind   = TOK_END;
    lx->cur.intV   = 0;
    lx->cur.floatV = 0.0;

    char c = *lx->src;
    if (c == '\0') { lx->cur.kind = TOK_END; return RC_OK; }

    switch (c) {
        case '(': lx->cur.kind = TOK_LPAREN; lx->src++; return RC_OK;
        case ')': lx->cur.kind = TOK_RPAREN; lx->src++; return RC_OK;
        case ',': lx->cur.kind = TOK_COMMA;  lx->src++; return RC_OK;
        case ';': lx->cur.kind = TOK_SEMI;   lx->src++; return RC_OK;
        case '*': lx->cur.kind = TOK_STAR;   lx->src++; return RC_OK;
        case '=': lx->cur.kind = TOK_EQ;     lx->src++; return RC_OK;
        case '<': lx->cur.kind = TOK_LT;     lx->src++; return RC_OK;
        case '>': lx->cur.kind = TOK_GT;     lx->src++; return RC_OK;
    }

    /* single-quoted string literal: 'foo' (doubled '' = escaped quote) */
    if (c == '\'') {
        lx->src++;                       /* skip opening quote */
        size_t cap = 16, len = 0;
        char *buf = (char *) malloc(cap);
        while (*lx->src) {
            if (*lx->src == '\'') {
                if (lx->src[1] == '\'') {
                    /* escaped quote */
                    if (len + 1 >= cap) { cap *= 2; buf = realloc(buf, cap); }
                    buf[len++] = '\'';
                    lx->src += 2;
                    continue;
                }
                break;                   /* closing quote */
            }
            if (len + 1 >= cap) { cap *= 2; buf = realloc(buf, cap); }
            buf[len++] = *lx->src++;
        }
        if (*lx->src == '\'') lx->src++; /* skip closing quote */
        buf[len] = '\0';
        lx->cur.kind = TOK_STRING;
        lx->cur.text = buf;
        return RC_OK;
    }

    /* numeric literal: integer or float */
    if (isdigit((unsigned char) c)) {
        int    iv = 0;
        double fv = 0.0;
        int    isFloat = 0;
        while (isdigit((unsigned char) *lx->src)) {
            iv = iv * 10 + (*lx->src - '0');
            lx->src++;
        }
        if (*lx->src == '.') {
            isFloat = 1;
            lx->src++;
            double scale = 0.1;
            while (isdigit((unsigned char) *lx->src)) {
                fv += (*lx->src - '0') * scale;
                scale *= 0.1;
                lx->src++;
            }
        }
        if (isFloat) {
            lx->cur.kind   = TOK_FLOAT;
            lx->cur.floatV = (double) iv + fv;
        } else {
            lx->cur.kind = TOK_INT;
            lx->cur.intV = iv;
        }
        return RC_OK;
    }

    /* identifier / keyword */
    if (isalpha((unsigned char) c) || c == '_') {
        const char *start = lx->src;
        while (isalnum((unsigned char) *lx->src) || *lx->src == '_') lx->src++;
        size_t len = (size_t)(lx->src - start);
        char *buf = (char *) malloc(len + 1);
        memcpy(buf, start, len);
        buf[len] = '\0';
        /* upper-case for keyword-insensitive comparison */
        for (size_t i = 0; i < len; i++)
            buf[i] = (char) toupper((unsigned char) buf[i]);
        lx->cur.kind = TOK_IDENT;
        lx->cur.text = buf;
        return RC_OK;
    }

    lx->cur.kind = TOK_UNKNOWN;
    lx->src++;   /* skip the bad char so the lexer can make progress */
    return RC_OK;
}

static void
lexInit(Lexer *lx, const char *src)
{
    lx->src = src;
    lx->cur.text = NULL;
    lx->cur.intV = 0;
    lx->cur.floatV = 0.0;
    lexNext(lx);
}

static void
lexDestroy(Lexer *lx)
{
    free(lx->cur.text);
    lx->cur.text = NULL;
}

/** Returns 1 if current token is the keyword `kw` (case-insensitive). */
static int
lexIs(Lexer *lx, const char *kw)
{
    return lx->cur.kind == TOK_IDENT
        && lx->cur.text != NULL
        && strcmp(lx->cur.text, kw) == 0;
}

/** Expect current token to be keyword `kw`; advance. */
static RC
lexExpect(Lexer *lx, const char *kw)
{
    if (!lexIs(lx, kw)) {
        fprintf(stderr, "[dml] expected '%s' but got '%s'\n",
                kw, lx->cur.text ? lx->cur.text : "<non-ident>");
        return RC_RM_INVALID_SCHEMA_DATA;
    }
    return lexNext(lx);
}

/* ================================================================== */
/*  Small helpers                                                     */
/* ================================================================== */

/* dynamic string array used while collecting column names */
typedef struct {
    char  **items;
    int     len;
    int     cap;
} StrVec;

static void svInit(StrVec *v) { v->items = NULL; v->len = 0; v->cap = 0; }

static RC svPush(StrVec *v, const char *s)
{
    if (v->len == v->cap) {
        int nc = v->cap ? v->cap * 2 : 8;
        char **ni = (char **) realloc(v->items, sizeof(char *) * nc);
        if (!ni) return RC_ALLOCATION_FAILED;
        v->items = ni; v->cap = nc;
    }
    char *dup = (char *) malloc(strlen(s) + 1);
    if (!dup) return RC_ALLOCATION_FAILED;
    strcpy(dup, s);
    v->items[v->len++] = dup;
    return RC_OK;
}

static void svFree(StrVec *v)
{
    for (int i = 0; i < v->len; i++) free(v->items[i]);
    free(v->items);
    svInit(v);
}

/* dynamic Value* array */
typedef struct {
    Value **items;
    int     len;
    int     cap;
} ValVec;

static void vvInit(ValVec *v) { v->items = NULL; v->len = 0; v->cap = 0; }

static RC vvPush(ValVec *v, Value *val)
{
    if (v->len == v->cap) {
        int nc = v->cap ? v->cap * 2 : 8;
        Value **ni = (Value **) realloc(v->items, sizeof(Value *) * nc);
        if (!ni) return RC_ALLOCATION_FAILED;
        v->items = ni; v->cap = nc;
    }
    v->items[v->len++] = val;
    return RC_OK;
}

static void vvFree(ValVec *v)
{
    for (int i = 0; i < v->len; i++) freeVal(v->items[i]);
    free(v->items);
    vvInit(v);
}

/**
 * Parse a scalar value literal and convert it to a Value* via the
 * stringToValue() encoding ("i123", "f1.5", "shello", "bt"/"bf").
 *
 *   integer    123       -> stringToValue("i123")
 *   float      1.5       -> stringToValue("f1.5")
 *   string     'hello'   -> stringToValue("shello")
 *   bool       TRUE/FALSE-> stringToValue("bt") / stringToValue("bf")
 */
static RC
parseValue(Lexer *lx, Value **out)
{
    char buf[256];

    switch (lx->cur.kind) {
        case TOK_INT:
            snprintf(buf, sizeof(buf), "i%d", lx->cur.intV);
            *out = stringToValue(buf);
            lexNext(lx);
            return RC_OK;

        case TOK_FLOAT:
            snprintf(buf, sizeof(buf), "f%g", lx->cur.floatV);
            *out = stringToValue(buf);
            lexNext(lx);
            return RC_OK;

        case TOK_STRING: {
            /* build "s" + text dynamically to avoid truncation */
            size_t n = strlen(lx->cur.text);
            char *enc = (char *) malloc(n + 2);
            if (!enc) return RC_ALLOCATION_FAILED;
            enc[0] = 's';
            memcpy(enc + 1, lx->cur.text, n + 1);
            *out = stringToValue(enc);
            free(enc);
            lexNext(lx);
            return RC_OK;
        }

        case TOK_IDENT:
            if (strcmp(lx->cur.text, "TRUE") == 0) {
                *out = stringToValue("bt");
                lexNext(lx);
                return RC_OK;
            }
            if (strcmp(lx->cur.text, "FALSE") == 0) {
                *out = stringToValue("bf");
                lexNext(lx);
                return RC_OK;
            }
            fprintf(stderr, "[dml] unexpected identifier '%s' as value\n",
                    lx->cur.text);
            return RC_RM_INVALID_SCHEMA_DATA;

        default:
            fprintf(stderr, "[dml] expected value literal\n");
            return RC_RM_INVALID_SCHEMA_DATA;
    }
}

/** Parse a column name (TOK_IDENT, but not a reserved keyword in context). */
static RC
parseColumnName(Lexer *lx, char **out)
{
    if (lx->cur.kind != TOK_IDENT) {
        fprintf(stderr, "[dml] expected column name\n");
        return RC_RM_INVALID_SCHEMA_DATA;
    }
    *out = (char *) malloc(strlen(lx->cur.text) + 1);
    if (!*out) return RC_ALLOCATION_FAILED;
    strcpy(*out, lx->cur.text);
    lexNext(lx);
    return RC_OK;
}

/** Parse a table name (TOK_IDENT). */
static RC
parseTableName(Lexer *lx, char **out)
{
    if (lx->cur.kind != TOK_IDENT) {
        fprintf(stderr, "[dml] expected table name\n");
        return RC_RM_INVALID_SCHEMA_DATA;
    }
    *out = (char *) malloc(strlen(lx->cur.text) + 1);
    if (!*out) return RC_ALLOCATION_FAILED;
    strcpy(*out, lx->cur.text);
    lexNext(lx);
    return RC_OK;
}

/* ================================================================== */
/*  WHERE expression                                                  */
/* ================================================================== */

/**
 * Parse a comparison predicate:  <col> ('='|'<'|'>') <val>
 *
 * The column reference is recorded as an EXPR_ATTRREF node with
 * attrRef = -1 (placeholder); the column name is appended to
 * `colNames` in the order it appears, so the executor can later
 * walk the Expr tree in-order and replace each -1 with the real
 * attribute index obtained from the catalog.
 *
 * '>' is implemented by swapping the operands and emitting an
 * OP_COMP_SMALLER (val < col), since expr.h only defines
 * OP_COMP_SMALLER.
 */
static RC
parseCmp(Lexer *lx, StrVec *colNames, Expr **out)
{
    RC rc;
    char *colName = NULL;
    rc = parseColumnName(lx, &colName); CHECKEX(rc);
    rc = svPush(colNames, colName);
    free(colName);          /* svPush made its own copy */
    if (rc != RC_OK) return rc;

    /* comparison operator */
    OpType op;
    int    swap = 0;
    switch (lx->cur.kind) {
        case TOK_EQ: op = OP_COMP_EQUAL;   swap = 0; break;
        case TOK_LT: op = OP_COMP_SMALLER; swap = 0; break;
        case TOK_GT: op = OP_COMP_SMALLER; swap = 1; break;
        default:
            fprintf(stderr, "[dml] expected comparison operator (=, <, >)\n");
            return RC_RM_INVALID_SCHEMA_DATA;
    }
    lexNext(lx);

    Value *val = NULL;
    rc = parseValue(lx, &val); CHECKEX(rc);

    Expr *attrRef;
    MAKE_ATTRREF(attrRef, -1);          /* placeholder, filled by executor */

    Expr *cons;
    MAKE_CONS(cons, val);

    if (swap) {
        /* col > val  ==>  val < col */
        Expr *result;
        MAKE_BINOP_EXPR(result, cons, attrRef, op);
        *out = result;
    } else {
        Expr *result;
        MAKE_BINOP_EXPR(result, attrRef, cons, op);
        *out = result;
    }
    return RC_OK;
}

/**
 * <factor> ::= [NOT] <cmp> | '(' <expr> ')'
 */
/* Forward declaration so parseFactor can recurse into a parenthesised
 * full OR-expression. */
static RC
parseOrExpr(Lexer *lx, StrVec *colNames, Expr **out);

static RC
parseFactor(Lexer *lx, StrVec *colNames, Expr **out)
{
    if (lexIs(lx, "NOT")) {
        lexNext(lx);
        Expr *child = NULL;
        RC rc = parseFactor(lx, colNames, &child);
        if (rc != RC_OK) return rc;
        Expr *result;
        MAKE_UNOP_EXPR(result, child, OP_BOOL_NOT);
        *out = result;
        return RC_OK;
    }

    if (lx->cur.kind == TOK_LPAREN) {
        lexNext(lx);
        Expr *inner = NULL;
        RC rc = parseOrExpr(lx, colNames, &inner);
        if (rc != RC_OK) return rc;
        if (lx->cur.kind != TOK_RPAREN) {
            fprintf(stderr, "[dml] expected ')' in expression\n");
            freeExpr(inner);
            return RC_RM_INVALID_SCHEMA_DATA;
        }
        lexNext(lx);
        *out = inner;
        return RC_OK;
    }

    return parseCmp(lx, colNames, out);
}

/** <term> ::= <factor> (AND <factor>)* */
static RC
parseAndExpr(Lexer *lx, StrVec *colNames, Expr **out)
{
    RC rc = parseFactor(lx, colNames, out);
    if (rc != RC_OK) return rc;

    while (lexIs(lx, "AND")) {
        lexNext(lx);
        Expr *rhs = NULL;
        rc = parseFactor(lx, colNames, &rhs);
        if (rc != RC_OK) { freeExpr(*out); *out = NULL; return rc; }
        Expr *combined;
        MAKE_BINOP_EXPR(combined, *out, rhs, OP_BOOL_AND);
        *out = combined;
    }
    return RC_OK;
}

/** <expr> ::= <term> (OR <term>)* */
static RC
parseOrExpr(Lexer *lx, StrVec *colNames, Expr **out)
{
    RC rc = parseAndExpr(lx, colNames, out);
    if (rc != RC_OK) return rc;

    while (lexIs(lx, "OR")) {
        lexNext(lx);
        Expr *rhs = NULL;
        rc = parseAndExpr(lx, colNames, &rhs);
        if (rc != RC_OK) { freeExpr(*out); *out = NULL; return rc; }
        Expr *combined;
        MAKE_BINOP_EXPR(combined, *out, rhs, OP_BOOL_OR);
        *out = combined;
    }
    return RC_OK;
}

/**
 * Parse an optional "WHERE <expr>" clause.  On return `*outExpr` is
 * NULL when no WHERE clause was present, otherwise the Expr tree.
 * Column names are appended to `colNames`.
 */
static RC
parseWhere(Lexer *lx, StrVec *colNames, Expr **outExpr)
{
    *outExpr = NULL;
    if (!lexIs(lx, "WHERE")) return RC_OK;
    lexNext(lx);
    return parseOrExpr(lx, colNames, outExpr);
}

/* ================================================================== */
/*  Statement parsers                                                 */
/* ================================================================== */

/**
 * SELECT ('*' | <col> (',' <col>)*) FROM <name> [WHERE <expr>] ';'
 */
static RC
parseSelect(Lexer *lx, DML_Statement *out)
{
    RC rc;

    /* column list */
    StrVec cols; svInit(&cols);
    if (lx->cur.kind == TOK_STAR) {
        out->selectAll = 1;
        lexNext(lx);
    } else {
        out->selectAll = 0;
        for (;;) {
            char *cn = NULL;
            rc = parseColumnName(lx, &cn); CHECKEX(rc);
            rc = svPush(&cols, cn);
            free(cn);
            if (rc != RC_OK) { svFree(&cols); return rc; }

            if (lx->cur.kind == TOK_COMMA) { lexNext(lx); continue; }
            break;
        }
    }

    CHECKEX(lexExpect(lx, "FROM"));

    char *tname = NULL;
    rc = parseTableName(lx, &tname);
    if (rc != RC_OK) { svFree(&cols); return rc; }
    out->tableName = tname;

    /* WHERE */
    StrVec whereCols; svInit(&whereCols);
    Expr *whereExpr = NULL;
    rc = parseWhere(lx, &whereCols, &whereExpr);
    if (rc != RC_OK) {
        svFree(&cols); svFree(&whereCols);
        if (whereExpr) freeExpr(whereExpr);
        return rc;
    }
    out->where         = whereExpr;
    out->whereColNames = whereCols.items;
    out->numWhereCols  = whereCols.len;

    /* optional ';' */
    if (lx->cur.kind == TOK_SEMI) lexNext(lx);

    out->type     = DML_SELECT;
    out->columns  = cols.items;
    out->numCols  = cols.len;
    return RC_OK;
}

/**
 * INSERT INTO <name> ['(' <col> (',' <col>)* ')']
 *        VALUES '(' <val> (',' <val>)* ')' ';'
 */
static RC
parseInsert(Lexer *lx, DML_Statement *out)
{
    RC rc;

    CHECKEX(lexExpect(lx, "INTO"));

    char *tname = NULL;
    rc = parseTableName(lx, &tname); CHECKEX(rc);
    out->tableName = tname;

    /* optional (col, col, ...) */
    StrVec cols; svInit(&cols);
    if (lx->cur.kind == TOK_LPAREN) {
        lexNext(lx);
        for (;;) {
            char *cn = NULL;
            rc = parseColumnName(lx, &cn); CHECKEX(rc);
            rc = svPush(&cols, cn);
            free(cn);
            if (rc != RC_OK) { svFree(&cols); return rc; }

            if (lx->cur.kind == TOK_COMMA) { lexNext(lx); continue; }
            if (lx->cur.kind == TOK_RPAREN) { lexNext(lx); break; }
            fprintf(stderr, "[dml] expected ',' or ')' in column list\n");
            svFree(&cols);
            return RC_RM_INVALID_SCHEMA_DATA;
        }
    }

    CHECKEX(lexExpect(lx, "VALUES"));

    if (lx->cur.kind != TOK_LPAREN) {
        fprintf(stderr, "[dml] expected '(' after VALUES\n");
        svFree(&cols);
        return RC_RM_INVALID_SCHEMA_DATA;
    }
    lexNext(lx);

    ValVec vals; vvInit(&vals);
    for (;;) {
        Value *v = NULL;
        rc = parseValue(lx, &v); CHECKEX(rc);
        rc = vvPush(&vals, v);
        if (rc != RC_OK) { freeVal(v); vvFree(&vals); svFree(&cols); return rc; }

        if (lx->cur.kind == TOK_COMMA) { lexNext(lx); continue; }
        if (lx->cur.kind == TOK_RPAREN) { lexNext(lx); break; }
        fprintf(stderr, "[dml] expected ',' or ')' in value list\n");
        vvFree(&vals); svFree(&cols);
        return RC_RM_INVALID_SCHEMA_DATA;
    }

    /* optional ';' */
    if (lx->cur.kind == TOK_SEMI) lexNext(lx);

    out->type      = DML_INSERT;
    out->columns   = cols.items;     /* may be NULL if no col list */
    out->numCols   = cols.len;
    out->values    = vals.items;
    out->numValues = vals.len;
    out->where         = NULL;
    out->whereColNames = NULL;
    out->numWhereCols  = 0;
    return RC_OK;
}

/**
 * UPDATE <name> SET <col>=<val> (',' <col>=<val>)* [WHERE <expr>] ';'
 */
static RC
parseUpdate(Lexer *lx, DML_Statement *out)
{
    RC rc;

    char *tname = NULL;
    rc = parseTableName(lx, &tname); CHECKEX(rc);
    out->tableName = tname;

    CHECKEX(lexExpect(lx, "SET"));

    StrVec setCols; svInit(&setCols);
    ValVec setVals; vvInit(&setVals);

    for (;;) {
        char *cn = NULL;
        rc = parseColumnName(lx, &cn); CHECKEX(rc);
        rc = svPush(&setCols, cn);
        free(cn);
        if (rc != RC_OK) { svFree(&setCols); vvFree(&setVals); return rc; }

        if (lx->cur.kind != TOK_EQ) {
            fprintf(stderr, "[dml] expected '=' in SET assignment\n");
            svFree(&setCols); vvFree(&setVals);
            return RC_RM_INVALID_SCHEMA_DATA;
        }
        lexNext(lx);

        Value *v = NULL;
        rc = parseValue(lx, &v); CHECKEX(rc);
        rc = vvPush(&setVals, v);
        if (rc != RC_OK) { freeVal(v); svFree(&setCols); vvFree(&setVals); return rc; }

        if (lx->cur.kind == TOK_COMMA) { lexNext(lx); continue; }
        break;
    }

    /* WHERE */
    StrVec whereCols; svInit(&whereCols);
    Expr *whereExpr = NULL;
    rc = parseWhere(lx, &whereCols, &whereExpr);
    if (rc != RC_OK) {
        svFree(&setCols); vvFree(&setVals); svFree(&whereCols);
        if (whereExpr) freeExpr(whereExpr);
        return rc;
    }
    out->where         = whereExpr;
    out->whereColNames = whereCols.items;
    out->numWhereCols  = whereCols.len;

    /* optional ';' */
    if (lx->cur.kind == TOK_SEMI) lexNext(lx);

    out->type    = DML_UPDATE;
    out->setCols = setCols.items;
    out->setVals = setVals.items;
    out->numSets = setCols.len;
    return RC_OK;
}

/**
 * DELETE FROM <name> [WHERE <expr>] ';'
 */
static RC
parseDelete(Lexer *lx, DML_Statement *out)
{
    RC rc;

    CHECKEX(lexExpect(lx, "FROM"));

    char *tname = NULL;
    rc = parseTableName(lx, &tname); CHECKEX(rc);
    out->tableName = tname;

    StrVec whereCols; svInit(&whereCols);
    Expr *whereExpr = NULL;
    rc = parseWhere(lx, &whereCols, &whereExpr);
    if (rc != RC_OK) {
        svFree(&whereCols);
        if (whereExpr) freeExpr(whereExpr);
        return rc;
    }
    out->where         = whereExpr;
    out->whereColNames = whereCols.items;
    out->numWhereCols  = whereCols.len;

    /* optional ';' */
    if (lx->cur.kind == TOK_SEMI) lexNext(lx);

    out->type = DML_DELETE;
    return RC_OK;
}

/* ================================================================== */
/*  Public API                                                        */
/* ================================================================== */

RC
parseDML(const char *sql, DML_Statement **out)
{
    if (!sql || !out) return RC_NULL_POINTER;
    *out = NULL;

    DML_Statement *st = (DML_Statement *) calloc(1, sizeof(DML_Statement));
    if (!st) return RC_ALLOCATION_FAILED;
    st->type          = DML_SELECT;     /* overwritten by dispatcher */
    st->tableName     = NULL;
    st->columns       = NULL;
    st->numCols       = 0;
    st->selectAll     = 0;
    st->where         = NULL;
    st->whereColNames = NULL;
    st->numWhereCols  = 0;
    st->values        = NULL;
    st->numValues     = 0;
    st->setCols       = NULL;
    st->setVals       = NULL;
    st->numSets       = 0;

    Lexer lx;
    lexInit(&lx, sql);

    RC rc;
    if (lexIs(&lx, "SELECT")) {
        lexNext(&lx);
        rc = parseSelect(&lx, st);
    } else if (lexIs(&lx, "INSERT")) {
        lexNext(&lx);
        rc = parseInsert(&lx, st);
    } else if (lexIs(&lx, "UPDATE")) {
        lexNext(&lx);
        rc = parseUpdate(&lx, st);
    } else if (lexIs(&lx, "DELETE")) {
        lexNext(&lx);
        rc = parseDelete(&lx, st);
    } else {
        fprintf(stderr,
                "[dml] statement must start with SELECT, INSERT, UPDATE or DELETE\n");
        rc = RC_RM_INVALID_SCHEMA_DATA;
    }

    lexDestroy(&lx);
    if (rc != RC_OK) {
        freeDMLStatement(st);
        return rc;
    }
    *out = st;
    return RC_OK;
}

RC
freeDMLStatement(DML_Statement *stmt)
{
    if (!stmt) return RC_OK;

    free(stmt->tableName);

    /* SELECT columns / INSERT column list */
    if (stmt->columns) {
        for (int i = 0; i < stmt->numCols; i++)
            free(stmt->columns[i]);
        free(stmt->columns);
    }

    /* WHERE Expr tree + column name list */
    if (stmt->where) freeExpr(stmt->where);
    if (stmt->whereColNames) {
        for (int i = 0; i < stmt->numWhereCols; i++)
            free(stmt->whereColNames[i]);
        free(stmt->whereColNames);
    }

    /* INSERT values */
    if (stmt->values) {
        for (int i = 0; i < stmt->numValues; i++)
            freeVal(stmt->values[i]);
        free(stmt->values);
    }

    /* UPDATE assignments */
    if (stmt->setCols) {
        for (int i = 0; i < stmt->numSets; i++)
            free(stmt->setCols[i]);
        free(stmt->setCols);
    }
    if (stmt->setVals) {
        for (int i = 0; i < stmt->numSets; i++)
            freeVal(stmt->setVals[i]);
        free(stmt->setVals);
    }

    free(stmt);
    return RC_OK;
}
