/**
 * @file ddl_parser.c
 * @authors Xingli Li
 * @date 2023-07-29
 * @brief Recursive-descent DDL parser implementation.
 *
 * The parser is intentionally small: a hand-written tokenizer splits the
 * input into tokens (keywords, identifiers, integers, punctuation), and
 * a recursive-descent driver assembles a DDL_Statement.
 *
 * Memory policy: every successful parseDDL() call returns a heap-allocated
 * DDL_Statement whose strings/arrays the caller releases with
 * freeDDLStatement().
 */
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <ctype.h>

#include "ddl_parser.h"
#include "btree_mgr.h"
#include "catalog.h"

/* ================================================================== */
/*  Tokenizer                                                        */
/* ================================================================== */

typedef enum {
    TOK_END,
    TOK_IDENT,        /* identifier or keyword (upper-cased) */
    TOK_NUMBER,       /* unsigned integer */
    TOK_LPAREN,       /* ( */
    TOK_RPAREN,       /* ) */
    TOK_COMMA,        /* , */
    TOK_SEMI,         /* ; */
    TOK_UNKNOWN
} TokKind;

typedef struct {
    TokKind kind;
    char   *text;       /* heap-allocated; for TOK_IDENT upper-cased */
    int     num;        /* for TOK_NUMBER */
} Token;

typedef struct {
    const char *src;    /* current read cursor */
    Token       cur;    /* current token */
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
    lx->cur.text = NULL;
    lx->cur.kind = TOK_END;
    lx->cur.num  = 0;

    char c = *lx->src;
    if (c == '\0') { lx->cur.kind = TOK_END; return RC_OK; }

    switch (c) {
        case '(': lx->cur.kind = TOK_LPAREN; lx->src++; return RC_OK;
        case ')': lx->cur.kind = TOK_RPAREN; lx->src++; return RC_OK;
        case ',': lx->cur.kind = TOK_COMMA;  lx->src++; return RC_OK;
        case ';': lx->cur.kind = TOK_SEMI;   lx->src++; return RC_OK;
    }

    if (isdigit((unsigned char) c)) {
        int v = 0;
        while (isdigit((unsigned char) *lx->src)) {
            v = v * 10 + (*lx->src - '0');
            lx->src++;
        }
        lx->cur.kind = TOK_NUMBER;
        lx->cur.num  = v;
        return RC_OK;
    }

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
        fprintf(stderr, "[ddl] expected '%s' but got '%s'\n",
                kw, lx->cur.text ? lx->cur.text : "<non-ident>");
        return RC_RM_INVALID_SCHEMA_DATA;
    }
    return lexNext(lx);
}

/* ================================================================== */
/*  Parser                                                           */
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

/** Parse one column definition: <name> <type> [( <len> )] */
static RC
parseColumn(Lexer *lx, StrVec *names, DataType *types, int *lengths, int idx)
{
    /* column name */
    if (lx->cur.kind != TOK_IDENT) {
        fprintf(stderr, "[ddl] expected column name\n");
        return RC_RM_INVALID_SCHEMA_DATA;
    }
    /* keep original case for the column name by re-reading from src? The
     * lexer upper-cased it; we accept that for simplicity. */
    RC rc = svPush(names, lx->cur.text);
    if (rc != RC_OK) return rc;
    lexNext(lx);

    /* type */
    if (lx->cur.kind != TOK_IDENT) {
        fprintf(stderr, "[ddl] expected column type\n");
        return RC_RM_INVALID_SCHEMA_DATA;
    }
    if (strcmp(lx->cur.text, "INT") == 0) {
        types[idx] = DT_INT;
        lengths[idx] = 0;
    } else if (strcmp(lx->cur.text, "FLOAT") == 0) {
        types[idx] = DT_FLOAT;
        lengths[idx] = 0;
    } else if (strcmp(lx->cur.text, "BOOL") == 0) {
        types[idx] = DT_BOOL;
        lengths[idx] = 0;
    } else if (strcmp(lx->cur.text, "STRING") == 0) {
        types[idx] = DT_STRING;
        lexNext(lx);
        if (lx->cur.kind != TOK_LPAREN) {
            fprintf(stderr, "[ddl] STRING requires (len)\n");
            return RC_RM_INVALID_SCHEMA_DATA;
        }
        lexNext(lx);
        if (lx->cur.kind != TOK_NUMBER) {
            fprintf(stderr, "[ddl] expected string length\n");
            return RC_RM_INVALID_SCHEMA_DATA;
        }
        lengths[idx] = lx->cur.num;
        lexNext(lx);
        if (lx->cur.kind != TOK_RPAREN) {
            fprintf(stderr, "[ddl] expected ')' after string length\n");
            return RC_RM_INVALID_SCHEMA_DATA;
        }
        lexNext(lx);
        return RC_OK;
    } else {
        fprintf(stderr, "[ddl] unknown type '%s'\n", lx->cur.text);
        return RC_RM_INVALID_SCHEMA_DATA;
    }
    lexNext(lx);
    return RC_OK;
}

/** Parse CREATE TABLE name ( col, col, ... ) [PRIMARY KEY (col)] ; */
static RC
parseCreate(Lexer *lx, DDL_Statement *out)
{
    CHECKEX(lexExpect(lx, "TABLE"));

    /* table name */
    if (lx->cur.kind != TOK_IDENT) {
        fprintf(stderr, "[ddl] expected table name\n");
        return RC_RM_INVALID_SCHEMA_DATA;
    }
    out->tableName = (char *) malloc(strlen(lx->cur.text) + 1);
    strcpy(out->tableName, lx->cur.text);
    lexNext(lx);

    /* '(' */
    if (lx->cur.kind != TOK_LPAREN) {
        fprintf(stderr, "[ddl] expected '(' after table name\n");
        return RC_RM_INVALID_SCHEMA_DATA;
    }
    lexNext(lx);

    /* column list */
    StrVec names; svInit(&names);
    /* growable arrays for types/lengths */
    int cap = 8, len = 0;
    DataType *types   = (DataType *) malloc(sizeof(DataType) * cap);
    int      *lengths = (int *)      malloc(sizeof(int) * cap);

    for (;;) {
        /* before parsing a column, check if we hit the PRIMARY KEY clause */
        if (lexIs(lx, "PRIMARY")) break;

        if (len == cap) {
            cap *= 2;
            types   = (DataType *) realloc(types,   sizeof(DataType) * cap);
            lengths = (int *)      realloc(lengths, sizeof(int)      * cap);
        }
        RC rc = parseColumn(lx, &names, types, lengths, len);
        if (rc != RC_OK) { svFree(&names); free(types); free(lengths); return rc; }
        len++;

        if (lx->cur.kind == TOK_COMMA) { lexNext(lx); continue; }
        if (lx->cur.kind == TOK_RPAREN) { lexNext(lx); break; }
        fprintf(stderr, "[ddl] expected ',' or ')' in column list\n");
        svFree(&names); free(types); free(lengths);
        return RC_RM_INVALID_SCHEMA_DATA;
    }

    /* optional PRIMARY KEY (col) */
    int pkAttr = -1;
    if (lexIs(lx, "PRIMARY")) {
        lexNext(lx);
        CHECKEX(lexExpect(lx, "KEY"));
        if (lx->cur.kind != TOK_LPAREN) {
            fprintf(stderr, "[ddl] expected '(' after PRIMARY KEY\n");
            svFree(&names); free(types); free(lengths);
            return RC_RM_INVALID_SCHEMA_DATA;
        }
        lexNext(lx);
        if (lx->cur.kind != TOK_IDENT) {
            fprintf(stderr, "[ddl] expected PK column name\n");
            svFree(&names); free(types); free(lengths);
            return RC_RM_INVALID_SCHEMA_DATA;
        }
        for (int i = 0; i < names.len; i++) {
            if (strcmp(names.items[i], lx->cur.text) == 0) { pkAttr = i; break; }
        }
        if (pkAttr < 0) {
            fprintf(stderr, "[ddl] PRIMARY KEY column '%s' not declared\n",
                    lx->cur.text);
            svFree(&names); free(types); free(lengths);
            return RC_RM_INVALID_SCHEMA_DATA;
        }
        lexNext(lx);
        if (lx->cur.kind != TOK_RPAREN) {
            fprintf(stderr, "[ddl] expected ')' after PK column\n");
            svFree(&names); free(types); free(lengths);
            return RC_RM_INVALID_SCHEMA_DATA;
        }
        lexNext(lx);
    }
    out->primaryKeyAttr = pkAttr;

    /* optional ';' */
    if (lx->cur.kind == TOK_SEMI) lexNext(lx);

    /* build schema */
    int *keys = NULL;
    int  keySize = (pkAttr >= 0) ? 1 : 0;
    if (keySize > 0) {
        keys = (int *) malloc(sizeof(int));
        keys[0] = pkAttr;
    }
    Schema *sch = createSchema(names.len, names.items, types, lengths,
                               keySize, keys);
    /* createSchema copies the values from types/lengths/keys into its own
     * malloc'd arrays, and stores the attrNames[i] pointers directly
     * (taking ownership of the strings). So we free the temporary arrays
     * but NOT the names.items[i] strings -- those now belong to sch. */
    free(types);
    free(lengths);
    free(keys);
    free(names.items);   /* the char* pointers themselves now belong to sch */
    names.items = NULL;

    out->schema = sch;
    out->type = DDL_CREATE_TABLE;
    return RC_OK;
}

static RC
parseDrop(Lexer *lx, DDL_Statement *out)
{
    CHECKEX(lexExpect(lx, "TABLE"));
    if (lx->cur.kind != TOK_IDENT) {
        fprintf(stderr, "[ddl] expected table name after DROP TABLE\n");
        return RC_RM_INVALID_SCHEMA_DATA;
    }
    out->tableName = (char *) malloc(strlen(lx->cur.text) + 1);
    strcpy(out->tableName, lx->cur.text);
    lexNext(lx);
    if (lx->cur.kind == TOK_SEMI) lexNext(lx);
    out->type = DDL_DROP_TABLE;
    out->schema = NULL;
    out->primaryKeyAttr = -1;
    return RC_OK;
}

RC
parseDDL(const char *sql, DDL_Statement **out)
{
    if (!sql || !out) return RC_NULL_POINTER;
    *out = NULL;

    DDL_Statement *st = (DDL_Statement *) calloc(1, sizeof(DDL_Statement));
    st->type = DDL_UNKNOWN;
    st->schema = NULL;
    st->tableName = NULL;
    st->primaryKeyAttr = -1;

    Lexer lx;
    lexInit(&lx, sql);

    RC rc;
    if (lexIs(&lx, "CREATE")) {
        lexNext(&lx);
        rc = parseCreate(&lx, st);
    } else if (lexIs(&lx, "DROP")) {
        lexNext(&lx);
        rc = parseDrop(&lx, st);
    } else {
        fprintf(stderr, "[ddl] statement must start with CREATE or DROP\n");
        rc = RC_RM_INVALID_SCHEMA_DATA;
    }

    lexDestroy(&lx);
    if (rc != RC_OK) {
        freeDDLStatement(st);
        return rc;
    }
    *out = st;
    return RC_OK;
}

RC
freeDDLStatement(DDL_Statement *stmt)
{
    if (!stmt) return RC_OK;
    if (stmt->schema)  freeSchema(stmt->schema);
    if (stmt->tableName) free(stmt->tableName);
    free(stmt);
    return RC_OK;
}

/* ================================================================== */
/*  executeDDL                                                        */
/* ================================================================== */

RC
executeDDL(const char *sql)
{
    if (!sql) return RC_NULL_POINTER;
    DDL_Statement *st = NULL;
    RC rc = parseDDL(sql, &st);
    if (rc != RC_OK) return rc;

    switch (st->type) {
        case DDL_CREATE_TABLE: {
            rc = initCatalog();
            if (rc != RC_OK) break;
            if (catalogLookupTable(st->tableName) != NULL) {
                rc = RC_RM_TABLE_EXISTS;
                break;
            }
            rc = createTable(st->tableName, st->schema);
            if (rc != RC_OK) break;

            /* if a primary key was declared, build an index file for it */
            int hasIdx = 0;
            char idxName[256];
            idxName[0] = '\0';
            if (st->primaryKeyAttr >= 0) {
                snprintf(idxName, sizeof(idxName), "%s.idx", st->tableName);
                DataType kt = st->schema->dataTypes[st->primaryKeyAttr];
                rc = createBTree(idxName, kt, 0);
                if (rc != RC_OK) {
                    deleteTable(st->tableName);
                    break;
                }
                hasIdx = 1;
            }
            rc = catalogRegisterTable(st->tableName, st->schema, hasIdx,
                                      hasIdx ? idxName : NULL);
            if (rc != RC_OK) {
                if (hasIdx) deleteBTree(idxName);
                deleteTable(st->tableName);
            }
            break;
        }
        case DDL_DROP_TABLE: {
            rc = initCatalog();
            if (rc != RC_OK) break;
            CatalogEntry *entry = catalogLookupTable(st->tableName);
            if (entry == NULL) {
                rc = RC_IM_KEY_NOT_FOUND;
                break;
            }
            char idxName[256];
            idxName[0] = '\0';
            if (entry->hasIndex && entry->indexName)
                snprintf(idxName, sizeof(idxName), "%s", entry->indexName);
            rc = deleteTable(st->tableName);
            if (rc != RC_OK) break;
            if (idxName[0] != '\0') {
                rc = deleteBTree(idxName);
                if (rc != RC_OK) break;
            }
            rc = catalogDropTable(st->tableName);
            break;
        }
        default:
            rc = RC_RM_INVALID_SCHEMA_DATA;
            break;
    }

    freeDDLStatement(st);
    return rc;
}
