//
// Created by Eric,wolfplus, Yang Li on 2023/7/14.
//
#include "dberror.h"
#include "storage_mgr.h"

#include <stdlib.h>
#include <string.h>

/* =========================================================================
 * Storage Manager - page-file layer
 *
 * A page file is treated as a sequence of fixed-size pages (PAGE_SIZE bytes
 * each). SM_FileHandle is a lightweight cursor: it remembers which file it
 * refers to, how many pages the file has, and the current page position.
 *
 * Key design points:
 *   - readBlock/writeBlock address pages by number; writeBlock implicitly
 *     grows the file by one page when writing right past the end
 *     (pageNum == totalNumPages).
 *   - appendEmptyBlock/ensureCapacity grow the file explicitly; the buffer
 *     manager uses them to extend tables on demand.
 *   - There is deliberately NO in-memory cache here. That is the buffer
 *     manager's job (chapter 2); this layer only does dumb block I/O.
 * ========================================================================= */

/* This function is used to initialize the storage manager */
void initStorageManager(void) {
    // No initialization required for this simple storage manager.
    // fp = NULL;
}

/* This function creates a new page file fileName. The initial file length should be one page. */
RC createPageFile(char *fileName) {
    FILE *fp = fopen(fileName, "w+"); // Open file in read/write mode, create it if it does not exist.
    if (fp == NULL)
        return RC_FILE_NOT_FOUND;

    SM_PageHandle *page = calloc(PAGE_SIZE, sizeof(char)); // Create an empty page.
    memset(page, '\0', PAGE_SIZE);
    fwrite(page, PAGE_SIZE, 1, fp);

    free(page);
    fclose(fp);
    return RC_OK;
}


/* This function opens an existing page file. */
RC openPageFile(char *fileName, SM_FileHandle *fHandle) {
    if (NULL == fileName) {
        return RC_FILE_NOT_FOUND;
    }
    if (NULL == fHandle) {
        return RC_FILE_HANDLE_NOT_INIT;
    }
    FILE *fp = fopen(fileName, "r+"); // Open file in read/write mode.
    if (fp == NULL)
        return RC_FILE_NOT_FOUND;
    fseek(fp, 0L, SEEK_END);
    int fileLength = ftell(fp);   // Calculate the total of pages in the file using a constant page size.
    rewind(fp);

    fHandle->fileName = fileName;
    fHandle->totalNumPages = fileLength / PAGE_SIZE;
    fHandle->curPagePos = 0;
    fHandle->mgmtInfo = fp;

    return RC_OK;
}

/* This function closes an open page file. */
RC closePageFile(SM_FileHandle *fHandle) {
    if (NULL == fHandle || NULL == fHandle->mgmtInfo)
        return RC_FILE_HANDLE_NOT_INIT;

    fHandle->fileName = NULL;
    fHandle->totalNumPages = 0;
    fHandle->curPagePos = -1;
    FILE *fp = fHandle->mgmtInfo;
    fclose(fp);
//    fclose(fp);

    return RC_OK;
}

/* This function destroys a page file. The file will no longer exist after running this method. */
RC destroyPageFile(char *fileName) {
//    fclose(fp);
    remove(fileName);
    return RC_OK;
}


/* Read a page from disk into memory. */
RC readBlock(int pageNum, SM_FileHandle *fHandle, SM_PageHandle memPage) {
    if (fHandle == NULL)
        return RC_FILE_HANDLE_NOT_INIT;

    if (pageNum < 0)
        return RC_READ_NON_EXISTING_PAGE;

    FILE *fp = fHandle->mgmtInfo;
    fseek(fp, pageNum * PAGE_SIZE, SEEK_SET);
    int pageSize = fread(memPage, sizeof(char), PAGE_SIZE, fp);
//    if (pageSize != PAGE_SIZE) {
//        return RC_READ_NON_EXISTING_PAGE;
//    }

    fHandle->curPagePos = ftell(fp) / PAGE_SIZE;
    return RC_OK;
}

/* Get the current block position. */
int getBlockPos(SM_FileHandle *fHandle) {
    if (fHandle == NULL)
        return RC_FILE_HANDLE_NOT_INIT;

    return fHandle->curPagePos;
}

/* Read the first block of the file. */
RC readFirstBlock(SM_FileHandle *fHandle, SM_PageHandle memPage) {
    return readBlock(0, fHandle, memPage);
}

/* Read the previous block relative to the current block position. */
RC readPreviousBlock(SM_FileHandle *fHandle, SM_PageHandle memPage) {
    if (fHandle == NULL)
        return RC_FILE_HANDLE_NOT_INIT;

    if (fHandle->curPagePos <= 0)
        return RC_READ_NON_EXISTING_PAGE;

    return readBlock(fHandle->curPagePos - 1, fHandle, memPage);
}

/* Read the block at the current block position. */
RC readCurrentBlock(SM_FileHandle *fHandle, SM_PageHandle memPage) {
    if (fHandle == NULL)
        return RC_FILE_HANDLE_NOT_INIT;

    return readBlock(fHandle->curPagePos, fHandle, memPage);
}

/* Read the next block relative to the current block position. */
RC readNextBlock(SM_FileHandle *fHandle, SM_PageHandle memPage) {
    if (fHandle == NULL)
        return RC_FILE_HANDLE_NOT_INIT;

    if (fHandle->curPagePos >= fHandle->totalNumPages - 1)
        return RC_READ_NON_EXISTING_PAGE;

    return readBlock(fHandle->curPagePos + 1, fHandle, memPage);
}

/* Read the last block of the file. */
RC readLastBlock(SM_FileHandle *fHandle, SM_PageHandle memPage) {
    if (fHandle == NULL)
        return RC_FILE_HANDLE_NOT_INIT;

    return readBlock(fHandle->totalNumPages - 1, fHandle, memPage);
}


/* Writes a page to disk from memory. */
RC writeBlock(int pageNum, SM_FileHandle *fHandle, SM_PageHandle memPage) {
    if (fHandle == NULL)
        return RC_FILE_HANDLE_NOT_INIT;

    if (pageNum < 0)
        return RC_WRITE_FAILED;

    FILE *fp = fHandle->mgmtInfo;
    fseek(fp, pageNum * PAGE_SIZE, SEEK_SET);

    if (fwrite(memPage, sizeof(char), PAGE_SIZE, fp) != PAGE_SIZE)
        return RC_WRITE_FAILED;

    fflush(fp);
    fHandle->curPagePos = ftell(fp) / PAGE_SIZE;
    if (pageNum == fHandle->totalNumPages) {
        fHandle->totalNumPages += 1;
    }
    return RC_OK;
}

/* Writes the current block to disk from memory. */
RC writeCurrentBlock(SM_FileHandle *fHandle, SM_PageHandle memPage) {
    if (fHandle == NULL)
        return RC_FILE_HANDLE_NOT_INIT;

    return writeBlock(fHandle->curPagePos, fHandle, memPage);
}

/* Appends an empty block to the end of a file. */
RC appendEmptyBlock(SM_FileHandle *fHandle) {
    if (fHandle == NULL)
        return RC_FILE_HANDLE_NOT_INIT;

    FILE *fp = fHandle->mgmtInfo;
    fseek(fp, 0L, SEEK_END);

    char *block = calloc(PAGE_SIZE, sizeof(char));
    if (fwrite(block, sizeof(char), PAGE_SIZE, fp) != PAGE_SIZE)
        return RC_WRITE_FAILED;

    fflush(fp);
    fHandle->totalNumPages += 1;
    free(block);
    return RC_OK;
}

/* If the file has less than numberOfPages pages then increase the size to numberOfPages. */
RC ensureCapacity(int numberOfPages, SM_FileHandle *fHandle) {
    if (fHandle == NULL)
        return RC_FILE_HANDLE_NOT_INIT;

    if (fHandle->totalNumPages < numberOfPages) {
        int additionalPages = numberOfPages - fHandle->totalNumPages;
        int i = 0;
        for (i = 0; i < additionalPages; i++) {
            if (appendEmptyBlock(fHandle) != RC_OK)
                return RC_WRITE_FAILED;
        }
    }
    return RC_OK;
}
