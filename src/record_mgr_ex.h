/**
 * @file record_mgr_ex.h
 * @authors Xingli Li
 * @date 2023-07-29
 * @brief Internal record manager metadata and serialization helpers.
 */
#ifndef RECORD_MGR_EX_H
#define RECORD_MGR_EX_H
#include <string.h>
#include <stdlib.h>
#include "record_mgr.h"
#include "storage_mgr.h"
#include "buffer_mgr.h"


#define MEMCPY_TO_OFFSET(__expression__, __type__)               \
    memcpy(&(buffer[offset]), __expression__, sizeof(__type__)); \
    offset += sizeof(__type__)

#define MEMCPY_FROM_OFFSET(__expression__, __type__)             \
    memcpy(__expression__, &(buffer[offset]), sizeof(__type__)); \
    offset += sizeof(__type__)

/**
 * Page Id slot Id relation
 *
 **/
typedef struct RM_PageSlot
{
    int page_id;
    int slot_id;
} PageSlot;

/**
 *  Table Info, which file handler and how many tuples
 **/
typedef struct RM_TableInfo
{
    //SM_FileHandle fh;
    int numOfTuples;
} TableInfo;

/**
 * 
 * expression scanner structure 
 *
 **/
typedef struct RM_Scanner
{
    int page;
    int slot;
    int lastPage;
    Expr *cond;
} Scanner;



#endif // RECORD_MGR_EX_H
