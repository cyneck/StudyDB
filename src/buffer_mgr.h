/**
 * @file buffer_mgr.h
 * @authors Xingli Li
 * @date 2023-07-29
 * @brief Buffer manager data structures and public interface.
 */
#ifndef BUFFER_MANAGER_H
#define BUFFER_MANAGER_H

#include "dberror.h"
#include "storage_mgr.h"
#include <stdlib.h>

#include "dt.h"


typedef enum ReplacementStrategy {
    RS_FIFO = 0,
    RS_LRU = 1,
    RS_CLOCK = 2,
    RS_LFU = 3,
    RS_LRU_K = 4
} ReplacementStrategy;


typedef int PageNumber;
#define NO_PAGE (-1)
typedef struct BM_BufferPool {
    char *pageFile;
    int numPages;
    bool isOpen;
    ReplacementStrategy strategy;
    void *mgmtData; /* private implementation, defined in buffer_mgr.c */
} BM_BufferPool;


typedef struct BM_PageHandle {
    PageNumber pageNum;
    char *data;
} BM_PageHandle;

// two macro
#define MAKE_POOL()                    \
  ((BM_BufferPool *) malloc (sizeof(BM_BufferPool)))

#define MAKE_PAGE_HANDLE()                \
  ((BM_PageHandle *) malloc (sizeof(BM_PageHandle)))

RC initBufferPool(BM_BufferPool *const bm, const char *const pageFileName,
                  const int numPages, ReplacementStrategy strategy,
                  void *stratData);

RC shutdownBufferPool(BM_BufferPool *const bm);

RC forceFlushPool(BM_BufferPool *const bm);

RC markDirty(BM_BufferPool *const bm, BM_PageHandle *const page);

RC unpinPage(BM_BufferPool *const bm, BM_PageHandle *const page);

RC forcePage(BM_BufferPool *const bm, BM_PageHandle *const page);

RC pinPage(BM_BufferPool *const bm, BM_PageHandle *const page,
           const PageNumber pageNum);

// Interface for statistic
PageNumber *getFrameContents(BM_BufferPool *const bm);

bool *getDirtyFlags(BM_BufferPool *const bm);

int *getFixCounts(BM_BufferPool *const bm);

int getNumReadIO(BM_BufferPool *const bm);

int getNumWriteIO(BM_BufferPool *const bm);

int getTotalNumPages(BM_BufferPool *const bm);

#endif
