/**
 * @file buffer_mgr_stat.h
 * @authors Xingli Li
 * @date 2023-07-29
 * @brief Buffer pool diagnostic helper interface.
 */
#ifndef BUFFER_MGR_STAT_H
#define BUFFER_MGR_STAT_H

#include "buffer_mgr.h"

// debug functions
void printPoolContent (BM_BufferPool *const bm);
void printPageContent (BM_PageHandle *const page);
char *sprintPoolContent (BM_BufferPool *const bm);
char *sprintPageContent (BM_PageHandle *const page);

#endif
