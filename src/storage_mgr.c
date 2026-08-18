/**
 * @file storage_mgr.c
 * @authors Xingli Li
 * @date 2023-07-29
 * @brief Page-file creation, block I/O, navigation, and capacity management.
 */

#include "storage_mgr.h"
//#include <stdlib.h>
#include <string.h>
//#include <error.h>

/**
 * Define the file header with 2 integers.
 * The first one is recording total number of pages which is 1 by default.
 * The second one is recording the page size which is 4096(2^12) by default.
 */
int fileHeader[2];

/**
 * @brief The initialization of the Storage Manager，initialize the file header.
 */
void initStorageManager(void) {
    printf("WELCOME TO DATABASE!\n");
    fileHeader[0] = 1;
    fileHeader[1] = PAGE_SIZE;
}

/**
 * @brief Create a new page file fileName. The initial file size should be one page.
 * This method should fill this single page with ’\0’ bytes.
 * @param fileName the name of page file
 * @return an integer return code also defined in dberror.h
 */
RC createPageFile(char *fileName) {
    int iRet = RC_OK;

    // open a binary file with "wb+" mode
    FILE *fp = fopen(fileName, "wb+");
    if (NULL == fp) {
        iRet = RC_FILE_NOT_FOUND;
    } else {
        // write the file header, after the file create
        if (fwrite(fileHeader, sizeof(int), 2, fp) != 2) {
            iRet = RC_WRITE_FAILED;
        }
        // the single page fill with '\0' bytes
        char fillPageZero[PAGE_SIZE] = {'\0'};
        if (fwrite(fillPageZero, sizeof(char), PAGE_SIZE, fp) != PAGE_SIZE) {
            iRet = RC_WRITE_FAILED;
        }

        fclose(fp);
    }

    return iRet;
}

/**
 * @brief Opens an existing page file
 * @param fileName the name of page file
 * @param fHandle A file handle represents an open page file
 * @return an integer return code also defined in dberror.h, return RC_FILE_NOT_FOUND if the file does not exist
 */
RC openPageFile(char *fileName, SM_FileHandle *fHandle) {
    int iRet = RC_OK;

    FILE *fp = fopen(fileName, "r+");
    if (NULL == fp) {
        iRet = RC_FILE_NOT_FOUND;
    } else {
        // assign page file info to attributes of the file handle
        fHandle->mgmtInfo = (void *) fp;
        fHandle->fileName = fileName;
        fHandle->curPagePos = 0;
        // init the total page number by reading the file header
        if (!fread(&fHandle->totalNumPages, sizeof(int), 1, fp)) {
            iRet = RC_FILE_HANDLE_NOT_INIT;
        }
    }
    return iRet;
}

/**
 * @brief close an open page file
 * @param fHandle A file handle represents an open page file
 * @return an integer return code also defined in dberror.h
 */
RC closePageFile(SM_FileHandle *fHandle) {
    int iRet = RC_OK;
    if (0 == fclose((FILE *) fHandle->mgmtInfo)) {
        // set the pointer to be NULL after close the file
        fHandle->mgmtInfo = NULL;
    } else {
        iRet = RC_FILE_NOT_FOUND;
    }

    return iRet;
}


/**
 * @brief destroy (delete) a page file
 * @param fileName the name of page file
 * @return an integer return code also defined in dberror.h
 */
RC destroyPageFile(char *fileName) {
    int iRet = RC_OK;
    if (remove(fileName) != 0) {
        iRet = RC_FILE_NOT_FOUND;
    }

    return iRet;
}


/**
 * @brief read the block at position pageNum from a file and
 * stores its content in the memory pointed to by the memPage page handle.
 * @param pageNum target reading position
 * @param fHandle a file handle represents an open page file
 * @param memPage a page handler to store target content in the memory
 * @return an integer return code also defined in dberror.h
 */
RC readBlock(int pageNum, SM_FileHandle *fHandle, SM_PageHandle memPage) {
    int iRet = RC_OK;
    // set the current page position to the target page number
    fHandle->curPagePos = pageNum;

    // move the file internal pointer to target position
    if (fseek((FILE *) fHandle->mgmtInfo, getBlockPos(fHandle), SEEK_SET)) {
        iRet = RC_READ_NON_EXISTING_PAGE;
    }
        // read the page data and store in the page handler
    else if (fread(memPage, PAGE_SIZE, 1, (FILE *) fHandle->mgmtInfo) != 1) {
        iRet = RC_READ_NON_EXISTING_PAGE;
    }

    return iRet;
}

/**
 * @brief get the current page position in a file
 * @param fHandle A file handle represents an open page file
 * @return the current page position in a file
 */
int getBlockPos(SM_FileHandle *fHandle) {
    // calculate the number of bytes in the position of the current block.
    // current block position = header size + (page size * current position)
    int iRet = sizeof(fileHeader) + (PAGE_SIZE * fHandle->curPagePos);
    return iRet;
}

/**
 * @brief Read the first page of the open file
 * @param fHandle A file handle represents an open page file
 * @param memPage a page handler to store target content in the memory
 * @return an integer return code also defined in dberror.h
 */
RC readFirstBlock(SM_FileHandle *fHandle, SM_PageHandle memPage) {
    return readBlock(0, fHandle, memPage);
}

/**
 * @brief Read the previous page of the open file relative to current page
 * @param fHandle A file handle represents an open page file
 * @param memPage a page handler to store target content in the memory
 * @return an integer return code also defined in dberror.h
 */
RC readPreviousBlock(SM_FileHandle *fHandle, SM_PageHandle memPage) {
    return readBlock(fHandle->curPagePos - 1, fHandle, memPage);
}

/**
 * @brief Read the current page of the open file
 * @param fHandle A file handle represents an open page file
 * @param memPage a page handler to store target content in the memory
 * @return an integer return code also defined in dberror.h
 */
RC readCurrentBlock(SM_FileHandle *fHandle, SM_PageHandle memPage) {
    return readBlock(fHandle->curPagePos, fHandle, memPage);
}

/**
 * @brief Read the next page of the open file relative to current page
 * @param fHandle A file handle represents an open page file
 * @param memPage a page handler to store target content in the memory
 * @return an integer return code also defined in dberror.h
 */
RC readNextBlock(SM_FileHandle *fHandle, SM_PageHandle memPage) {
    return readBlock(fHandle->curPagePos + 1, fHandle, memPage);
}

/**
 * @brief Read the last page of the open file
 * @param fHandle A file handle represents an open page file
 * @param memPage a page handler to store target content in the memory
 * @return an integer return code also defined in dberror.h
 */
RC readLastBlock(SM_FileHandle *fHandle, SM_PageHandle memPage) {
    return readBlock(fHandle->totalNumPages - 1, fHandle, memPage);
}


/**
 * @brief write a page to disk with target position
 * @param pageNum target writing position
 * @param fHandle A file handle represents an open page file
 * @param memPage a page handler to store target content in the memory
 * @return an integer return code also defined in dberror.h
 */
RC writeBlock(int pageNum, SM_FileHandle *fHandle, SM_PageHandle memPage) {
    int iRet = RC_OK;

    // set the current page position to the target page number
    fHandle->curPagePos = pageNum;

    // move the file internal pointer to target position
    if (fseek((FILE *) fHandle->mgmtInfo, getBlockPos(fHandle), SEEK_SET)) {
        iRet = RC_WRITE_FAILED;
    }
        // write the page data to the target block
    else if (fwrite(memPage, sizeof(char), PAGE_SIZE, (FILE *) fHandle->mgmtInfo) != PAGE_SIZE) {
        iRet = RC_WRITE_FAILED;
    }

    return iRet;
}

/**
 * @brief write a page to disk with current position
 * @param fHandle A file handle represents an open page file
 * @param memPage a page handler to store target content in the memory
 * @return an integer return code also defined in dberror.h
 */
RC writeCurrentBlock(SM_FileHandle *fHandle, SM_PageHandle memPage) {
    return writeBlock(fHandle->curPagePos, fHandle, memPage);
}

/**
 * @brief Increase the number of pages in the file by one with zero bytes
 * @param fHandle A file handle represents an open page file
 * @return an integer return code also defined in dberror.h
 */
RC appendEmptyBlock(SM_FileHandle *fHandle) {
    int iRet = RC_OK;
    char fillPageZero[PAGE_SIZE] = {'\0'};

    // increase the total page number by 1
    fHandle->totalNumPages += 1;

    // move the file internal pointer to end position
    if (fseek((FILE *) fHandle->mgmtInfo, 0, SEEK_SET)) {
        iRet = RC_WRITE_FAILED;
    } else if (fwrite(&fHandle->totalNumPages, sizeof(int), 2, (FILE *) fHandle->mgmtInfo) != 2) {
        iRet = RC_WRITE_FAILED;
    }

    if (fseek((FILE *) fHandle->mgmtInfo, 0, SEEK_END)) {
        iRet = RC_WRITE_FAILED;
    } else if (fwrite(fillPageZero, sizeof(char), PAGE_SIZE, (FILE *) fHandle->mgmtInfo) != PAGE_SIZE) {
        iRet = RC_WRITE_FAILED;
    }

    return iRet;
}

/**
 * @brief If the file has less than numberOfPages pages then increase the size to numberOfPages
 * @param numberOfPages
 * @param fHandle A file handle represents an open page file
 * @return an integer return code also defined in dberror.h
 */
RC ensureCapacity(int numberOfPages, SM_FileHandle *fHandle) {
    int iRet = RC_OK;
    while (numberOfPages > fHandle->totalNumPages && iRet == RC_OK) {
        iRet = appendEmptyBlock(fHandle);
    }

    return iRet;
}

