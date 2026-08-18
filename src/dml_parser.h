/**
 * @file dml_parser.h
 * @authors Xingli Li
 * @date 2023-07-29
 * @brief DML (Data Manipulation Language) parser.
 *
 * Supported grammar (case-insensitive keywords, ';' terminated):
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
 * WHERE expressions reuse the Expr tree from expr.h so the existing
 * evalExpr() can evaluate them directly.
 *
 * Note: '>' is supported syntactically but implemented by swapping
 * operands to '<' (val < col), since expr.h only defines OP_COMP_SMALLER.
 */
#ifndef DML_PARSER_H
#define DML_PARSER_H

#include "dberror.h"
#include "tables.h"
#include "expr.h"

typedef enum {
    DML_SELECT,
    DML_INSERT,
    DML_UPDATE,
    DML_DELETE
} DML_StmtType;

typedef struct DML_Statement {
    DML_StmtType type;
    char *tableName;

    /* SELECT */
    char **columns;     /* NULL means SELECT * */
    int    numCols;
    int    selectAll;   /* 1 if SELECT * */

    /* WHERE (shared by SELECT/UPDATE/DELETE) */
    Expr   *where;      /* NULL if no WHERE clause. Column references
                         * inside the tree use EXPR_ATTRREF with
                         * attrRef = -1 as a placeholder; the actual
                         * attribute index is filled in later by the
                         * executor after consulting the catalog. The
                         * corresponding column names are stored below
                         * in the order they appear in an in-order
                         * traversal of the Expr tree, so the executor
                         * can match each -1 ATTRREF to its name. */
    char  **whereColNames;
    int     numWhereCols;

    /* INSERT */
    Value **values;     /* column values in order */
    int     numValues;

    /* UPDATE */
    char  **setCols;    /* column names to set */
    Value **setVals;    /* new values */
    int     numSets;
} DML_Statement;

/** Parse a DML statement string. Returns heap-allocated statement. */
extern RC parseDML(const char *sql, DML_Statement **out);

/** Free a statement returned by parseDML. */
extern RC freeDMLStatement(DML_Statement *stmt);

#endif /* DML_PARSER_H */
