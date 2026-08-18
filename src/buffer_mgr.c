#include "buffer_mgr.h"
#include "storage_mgr.h"
#include <stdlib.h>
#include <string.h>

/* =========================================================================
 * Buffer Manager - page cache in front of the storage manager
 *
 * Design (differs from the classic hash-table implementation):
 *   - The pool is a doubly linked list of BufferFrames, each holding one
 *     page copy, its dirty flag, fix (pin) count, and two timestamps:
 *         entryTimestamp   -> age since the frame was loaded  (FIFO)
 *         accessTimestamp  -> time of last access             (LRU)
 *   - Lookup is a linear walk of the list. This is O(pool size) instead of
 *     O(1) hashing, but the pool sizes used here are small and the simple
 *     structure is much easier to follow when studying the code.
 *   - pinPage brings a page in on demand and grows the underlying file when
 *     the requested page does not exist yet (appendEmptyBlock).
 *   - Only FIFO and LRU are implemented; the other strategies return -1.
 * ========================================================================= */

// Structure to hold information about each page frame in the buffer pool
typedef struct BufferFrame
{
    BM_PageHandle *pageHandle; // Pointer to the page handle
    bool dirty;                // Flag indicating if the page is dirty
    int fixCount;              // Fix count of the page
    int accessTimestamp;       // Timestamp for LRU
    int entryTimestamp;        // Timestamp for FIFO
    struct BufferFrame *prev;  // Pointer to the previous frame
    struct BufferFrame *next;  // Pointer to the next frame
} BufferFrame;

// Structure to hold information about the buffer pool
typedef struct BufferPool
{
    SM_FileHandle *fileHandle;     // Pointer to the file handle
    BM_BufferPool *bufferPoolInfo; // Pointer to the buffer pool information
    BufferFrame *bufferFrames;     // Linked list of buffer frames
    int numReadIO;                 // Counter for read I/O operations
    int numWriteIO;                // Counter for write I/O operations
    int currentEmptyFrame;         // Index of the current empty frame
    int currentPagePosition;       // Position of the current page in the file
    int entryTimestamp;          // Current timestamp for FIFO
    int accessTimestamp;           // Access timestamp for LRU
} BufferPool;

// Function to find the page frame to replace using FIFO strategy
int findFIFOPageFrame(BufferPool *bufferPool)
{
    int frameIndex = -1;
    int minTimestamp = bufferPool->entryTimestamp;
    // Find the first page frame with fix count 0
    BufferFrame *currentFrame = bufferPool->bufferFrames;
    while (currentFrame != NULL)
    {
        if (currentFrame->fixCount == 0 && currentFrame->entryTimestamp < minTimestamp)
        {
            minTimestamp = currentFrame->entryTimestamp;
            frameIndex = currentFrame->pageHandle->pageNum;
        }
        currentFrame = currentFrame->next;
    }

    return frameIndex;
}

// Function to find the page frame to replace using LRU strategy
int findLRUPageFrame(BufferPool *bufferPool)
{
    int frameIndex = -1;
    int minTimestamp = bufferPool->accessTimestamp;

    // Find the least recently used page frame
    BufferFrame *currentFrame = bufferPool->bufferFrames;
    while (currentFrame != NULL)
    {
        if (currentFrame->fixCount == 0 && currentFrame->accessTimestamp < minTimestamp)
        {
            minTimestamp = currentFrame->accessTimestamp;
            frameIndex = currentFrame->pageHandle->pageNum;
        }
        currentFrame = currentFrame->next;
    }

    return frameIndex;
}

// Function to update LRU timestamps when a page is accessed or pinned
void updateLRUTimestamps(BufferPool *bufferPool, const PageNumber pageNum)
{
    BufferFrame *currentFrame = bufferPool->bufferFrames;
    while (currentFrame != NULL)
    {
        if (currentFrame->pageHandle->pageNum == pageNum)
        {
            currentFrame->accessTimestamp = bufferPool->accessTimestamp++;
            break;
        }
        currentFrame = currentFrame->next;
    }
}

// Function to initialize a buffer pool
RC initBufferPool(BM_BufferPool *const bm, const char *const pageFileName,
                  const int numPages, ReplacementStrategy strategy, void *stratData)
{
    // Allocate memory for the buffer pool
    BufferPool *bufferPool = (BufferPool *)malloc(sizeof(BufferPool));
    if (bufferPool == NULL)
        return RC_BM_POOL_INIT_FAILED;

    // Open the page file
    SM_FileHandle *fileHandle = (SM_FileHandle *)malloc(sizeof(SM_FileHandle));
    if (openPageFile(pageFileName, fileHandle) != RC_OK)
        return RC_FILE_NOT_FOUND;

    // Set the buffer pool information
    bufferPool->fileHandle = fileHandle;
    bufferPool->bufferPoolInfo = bm;
    bufferPool->bufferFrames = NULL;
    bufferPool->currentEmptyFrame = 0;
    bufferPool->currentPagePosition = -1;
    bufferPool->entryTimestamp = 0;
    bufferPool->accessTimestamp = 0;
    bufferPool->numReadIO = 0;
    bufferPool->numWriteIO = 0;

    // Set the buffer pool in the BM_BufferPool structure
    bm->pageFile = (char *)pageFileName;
    bm->numPages = numPages;
    bm->strategy = strategy;
    // Cast through void* so this compiles against headers where mgmtData
    // is typed as BM_MgmtData * (StudyDB) as well as void * (course skeleton).
    bm->mgmtData = (void *)bufferPool;

    return RC_OK;
}

// Function to shutdown a buffer pool
RC shutdownBufferPool(BM_BufferPool *const bm)
{
    // Get the buffer pool from the BM_BufferPool structure
    BufferPool *bufferPool = (BufferPool *)bm->mgmtData;

    // Check if there are any pinned pages in the buffer pool
    BufferFrame *currentFrame = bufferPool->bufferFrames;
    while (currentFrame != NULL)
    {
        if (currentFrame->fixCount > 0)
            return RC_BM_PINNED_PAGES_EXIST;
        currentFrame = currentFrame->next;
    }

    // Write all dirty pages to disk
    forceFlushPool(bm);

    // Close the page file
    if (closePageFile(bufferPool->fileHandle) != RC_OK)
        return RC_FILE_HANDLE_NOT_INIT;

    // Free memory allocated for buffer frames
    while (bufferPool->bufferFrames != NULL)
    {
        BufferFrame *nextFrame = bufferPool->bufferFrames->next;
        free(bufferPool->bufferFrames->pageHandle->data);
        free(bufferPool->bufferFrames->pageHandle);
        free(bufferPool->bufferFrames);
        bufferPool->bufferFrames = nextFrame;
    }

    // Free memory allocated for buffer pool
    free(bufferPool->fileHandle); //todo: why this line cause error?
    free(bufferPool);

    return RC_OK;
}

// Function to flush all dirty pages in the buffer pool to disk
RC forceFlushPool(BM_BufferPool *const bm)
{
    // Get the buffer pool from the BM_BufferPool structure
    BufferPool *bufferPool = (BufferPool *)bm->mgmtData;

    // Write all dirty pages to disk
    BufferFrame *currentFrame = bufferPool->bufferFrames;
    while (currentFrame != NULL)
    {
        if (currentFrame->dirty)
        {
            if (writeBlock(currentFrame->pageHandle->pageNum, bufferPool->fileHandle,
                           currentFrame->pageHandle->data) != RC_OK)
                return RC_WRITE_FAILED;

            currentFrame->dirty = false;
            bufferPool->numWriteIO++;
        }
        currentFrame = currentFrame->next;
    }

    return RC_OK;
}

// Function to mark a page as dirty
RC markDirty(BM_BufferPool *const bm, BM_PageHandle *const page)
{
    // Get the buffer pool from the BM_BufferPool structure
    BufferPool *bufferPool = (BufferPool *)bm->mgmtData;

    // Find the page frame for the given page
    BufferFrame *currentFrame = bufferPool->bufferFrames;
    while (currentFrame != NULL)
    {
        if (currentFrame->pageHandle->pageNum == page->pageNum)
        {
            currentFrame->dirty = true;
            return RC_OK;
        }
        currentFrame = currentFrame->next;
    }

    return RC_BM_PAGE_NOT_FOUND;
}

// Function to unpin a page
RC unpinPage(BM_BufferPool *const bm, BM_PageHandle *const page)
{
    // Get the buffer pool from the BM_BufferPool structure
    BufferPool *bufferPool = (BufferPool *)bm->mgmtData;

    // Find the page frame for the given page
    BufferFrame *currentFrame = bufferPool->bufferFrames;
    while (currentFrame != NULL)
    {
        if (currentFrame->pageHandle->pageNum == page->pageNum)
        {
            currentFrame->fixCount--;

            // If fix count becomes 0, update page position
            if (currentFrame->fixCount == 0)
                bufferPool->currentPagePosition = currentFrame->pageHandle->pageNum;

            // Update LRU timestamp
            updateLRUTimestamps(bufferPool, page->pageNum);

            return RC_OK;
        }
        currentFrame = currentFrame->next;
    }

    return RC_BM_PAGE_NOT_FOUND;
}

// Function to write a page back to disk
RC forcePage(BM_BufferPool *const bm, BM_PageHandle *const page)
{
    // Get the buffer pool from the BM_BufferPool structure
    BufferPool *bufferPool = (BufferPool *)bm->mgmtData;

    // Find the page frame for the given page
    BufferFrame *currentFrame = bufferPool->bufferFrames;
    while (currentFrame != NULL)
    {
        if (currentFrame->pageHandle->pageNum == page->pageNum)
        {
            if (writeBlock(currentFrame->pageHandle->pageNum, bufferPool->fileHandle,
                           currentFrame->pageHandle->data) != RC_OK)
                return RC_WRITE_FAILED;

            currentFrame->dirty = false;
            bufferPool->numWriteIO++;

            // Update LRU timestamp
            updateLRUTimestamps(bufferPool, page->pageNum);

            return RC_OK;
        }
        currentFrame = currentFrame->next;
    }

    return RC_BM_PAGE_NOT_FOUND;
}

// Function to pin a page
RC pinPage(BM_BufferPool *const bm, BM_PageHandle *const page, const PageNumber pageNum)
{
    // Get the buffer pool from the BM_BufferPool structure
    BufferPool *bufferPool = (BufferPool *)bm->mgmtData;

    // Check if the page is already in a page frame
    BufferFrame *currentFrame = bufferPool->bufferFrames;
    while (currentFrame != NULL)
    {
        if (currentFrame->pageHandle->pageNum == pageNum)
        {
            // Update fix count and LRU timestamp, then return the page frame
            currentFrame->fixCount++;
            updateLRUTimestamps(bufferPool, pageNum);
            page->data = currentFrame->pageHandle->data;
            page->pageNum = pageNum;
            bufferPool->currentPagePosition = pageNum;
            return RC_OK;
        }
        currentFrame = currentFrame->next;
    }

    // Page is not in any page frame, need to bring it from disk
    // Check if there is an empty frame available
    if (bufferPool->currentEmptyFrame < bm->numPages)
    {
        // Allocate memory for the page handle and page data
        BM_PageHandle *newPageHandle = (BM_PageHandle *)malloc(sizeof(BM_PageHandle));
        newPageHandle->data = (char *)calloc(PAGE_SIZE, sizeof(char));
        newPageHandle->pageNum = pageNum;

        if (pageNum >= bufferPool->fileHandle->totalNumPages)
        {
            appendEmptyBlock(bufferPool->fileHandle);
        }

        // Read the page from disk
        if (readBlock(pageNum, bufferPool->fileHandle, newPageHandle->data) != RC_OK)
        {
            free(newPageHandle->data);
            free(newPageHandle);
            return RC_READ_NON_EXISTING_PAGE;
        }
        bufferPool->numReadIO++;
        // Create a new buffer frame
        BufferFrame *newFrame = (BufferFrame *)malloc(sizeof(BufferFrame));
        newFrame->pageHandle = newPageHandle;
        newFrame->dirty = false;
        newFrame->fixCount = 1;
        newFrame->accessTimestamp = bufferPool->numReadIO + bufferPool->numWriteIO;
        newFrame->entryTimestamp = bufferPool->entryTimestamp++;
        newFrame->prev = NULL;
        newFrame->next = NULL;

        // Add the new frame to the linked list of buffer frames
        if (bufferPool->bufferFrames == NULL)
        {
            bufferPool->bufferFrames = newFrame;
        }
        else
        {
            BufferFrame *lastFrame = bufferPool->bufferFrames;
            while (lastFrame->next != NULL)
            {
                lastFrame = lastFrame->next;
            }
            lastFrame->next = newFrame;
            newFrame->prev = lastFrame;
        }

        // Return the page frame
        page->data = newPageHandle->data;
        page->pageNum = pageNum;

        // Update empty frame index
        bufferPool->currentEmptyFrame++;
        bufferPool->currentPagePosition = pageNum;

        return RC_OK;
    }

    // All page frames are occupied, need to replace a page using the chosen strategy
    int frameIndex;
    if (bm->strategy == RS_FIFO)
    {
        frameIndex = findFIFOPageFrame(bufferPool);
    }
    else if (bm->strategy == RS_LRU)
    {
        frameIndex = findLRUPageFrame(bufferPool);
    }
    else
    {
        return -1;
    }

    // If a page frame is found for replacement
    if (frameIndex >= 0)
    {
        // Find the page frame to be replaced
        BufferFrame *replaceFrame = bufferPool->bufferFrames;
        while (replaceFrame != NULL)
        {
            if (replaceFrame->pageHandle->pageNum == frameIndex)
            {
                // Write the current page in the frame back to disk if it's dirty
                if (replaceFrame->dirty)
                {
                    if (writeBlock(replaceFrame->pageHandle->pageNum, bufferPool->fileHandle,
                                   replaceFrame->pageHandle->data) != RC_OK)
                        return RC_WRITE_FAILED;

                    replaceFrame->dirty = false;
                    bufferPool->numWriteIO++;
                }

                if (pageNum >= bufferPool->fileHandle->totalNumPages)
                {
                    appendEmptyBlock(bufferPool->fileHandle);
                }
                bufferPool->numReadIO++;

                // Read the new page from disk
                if (readBlock(pageNum, bufferPool->fileHandle, replaceFrame->pageHandle->data) != RC_OK)
                    return RC_READ_NON_EXISTING_PAGE;

                replaceFrame->pageHandle->pageNum = pageNum;
                replaceFrame->fixCount = 1;
                replaceFrame->accessTimestamp = bufferPool->numReadIO + bufferPool->numWriteIO;
                replaceFrame->entryTimestamp = bufferPool->entryTimestamp++;

                // Update LRU timestamp
                updateLRUTimestamps(bufferPool, pageNum);


                // Return the page frame
                page->data = replaceFrame->pageHandle->data;
                page->pageNum = pageNum;

                bufferPool->currentPagePosition = pageNum;

                return RC_OK;
            }
            replaceFrame = replaceFrame->next;
        }
    }

    return RC_BM_BUFFER_POOL_FULL;
}

// Function to get the content of each page frame in the buffer pool
PageNumber *getFrameContents(BM_BufferPool *const bm)
{
    // Get the buffer pool from the BM_BufferPool structure
    BufferPool *bufferPool = (BufferPool *)bm->mgmtData;

    PageNumber *frameContents = (PageNumber *)malloc(sizeof(PageNumber) * bm->numPages);
    memset(frameContents, NO_PAGE, sizeof(PageNumber) * bm->numPages);
    int i = 0;
    BufferFrame *currentFrame = bufferPool->bufferFrames;
    while (currentFrame != NULL)
    {
        frameContents[i] = currentFrame->pageHandle->pageNum;
        currentFrame = currentFrame->next;
        i++;
    }
    return frameContents;
}

// Function to get the dirty flags of each page frame in the buffer pool
bool *getDirtyFlags(BM_BufferPool *const bm)
{
    // Get the buffer pool from the BM_BufferPool structure
    BufferPool *bufferPool = (BufferPool *)bm->mgmtData;

    bool *dirtyFlags = (bool *)malloc(sizeof(bool) * bm->numPages);
    memset(dirtyFlags, false, sizeof(bool) * bm->numPages);
    int i = 0;
    BufferFrame *currentFrame = bufferPool->bufferFrames;
    while (currentFrame != NULL)
    {
        dirtyFlags[i] = currentFrame->dirty;
        currentFrame = currentFrame->next;
        i++;
    }
    return dirtyFlags;
}

// Function to get the fix counts of each page frame in the buffer pool
int *getFixCounts(BM_BufferPool *const bm)
{
    // Get the buffer pool from the BM_BufferPool structure
    BufferPool *bufferPool = (BufferPool *)bm->mgmtData;

    int *fixCounts = (int *)malloc(sizeof(int) * bm->numPages);
    memset(fixCounts, 0, sizeof(int) * bm->numPages);
    int i = 0;
    BufferFrame *currentFrame = bufferPool->bufferFrames;
    while (currentFrame != NULL)
    {
        fixCounts[i] = currentFrame->fixCount;
        currentFrame = currentFrame->next;
        i++;
    }
    return fixCounts;
}

// Function to get the number of pages that have been read from disk
int getNumReadIO(BM_BufferPool *const bm)
{
    // Get the buffer pool from the BM_BufferPool structure
    BufferPool *bufferPool = (BufferPool *)bm->mgmtData;

    return bufferPool->numReadIO;
}

// Function to get the number of pages that have been written to disk
int getNumWriteIO(BM_BufferPool *const bm)
{
    // Get the buffer pool from the BM_BufferPool structure
    BufferPool *bufferPool = (BufferPool *)bm->mgmtData;

    return bufferPool->numWriteIO;
}

// Function to get the total number of pages in the underlying page file
// (used by the record manager to bound scans on tables larger than the pool)
int getTotalNumPages(BM_BufferPool *const bm)
{
    BufferPool *bufferPool = (BufferPool *)bm->mgmtData;
    if (bufferPool == NULL || bufferPool->fileHandle == NULL)
        return 0;
    return bufferPool->fileHandle->totalNumPages;
}
