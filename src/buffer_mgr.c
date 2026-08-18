/**
 * @file buffer_mgr.c
 * @authors Xingli Li
 * @date 2023-07-29
 * @brief Buffer pool and page replacement strategy implementation.
 */
#include "buffer_mgr.h"
#include <stdio.h>
#include <string.h>
#include <math.h>
#include <assert.h>

/**
 * @brief calculate the hash value of a key,
 * the hash algorithm is base on the book
 * [Introduction.to.Algorithms].Thomas.H.Cormen.Ronald.L.Rivest.Charles.E.Leiserson.Clifford.Stein,
 * Chapter 11 Hash Tables.
 * @param hTable the table to store the values.
 * @param key the original key of the hash table.
 * @return hashed key value.
 */
size_t hashOfKey(HashTable *hTable, int key) {
    double a = (sqrt(5) - 1) / 2;
    int wSize = 32;
    unsigned int s = (unsigned) floor(a * 2 * ((unsigned int) 2 << (wSize - 2)));
    unsigned int x = key * s;
    int p = (int) (log(hTable->size) / log(2));
    size_t hashValue = x >> (wSize - p);

    // Ensure the hashValue will not overflow of the hash table size
    if (hashValue > hTable->size) {
        hashValue = hashValue % hTable->size;
    }
    return hashValue;
}


/**
 * @brief create a hash table base on the given table size.
 * @param tableSize the target size of the hash table.
 * @return An empty size limited hash table.
 */
HashTable *createHashTable(size_t tableSize) {

    HashTable *table = (HashTable *) malloc(sizeof(HashTable));

    table->hashNode = malloc(sizeof(HashNode) * tableSize);
    table->size = tableSize;
    size_t i = 0;
    for ( i = 0; i < tableSize; ++i) {
        table->hashNode[i].key = NO_KEY;
        table->hashNode[i].next = NULL;
    }
    return table;
}

/**
 * @brief insert a key-value pair into the given hash table.
 * @param hashTable the target hash table.
 * @param key the target key.
 * @param value the target value refer to the key.
 */
void insertHashNode(HashTable *hashTable, int key, int value) {
    size_t hashValue = hashOfKey(hashTable, key);
    // create a new node if the key is not exist in the hash table.
    if (hashTable->hashNode[hashValue].key == NO_KEY) {
        hashTable->hashNode[hashValue].key = key;
        hashTable->hashNode[hashValue].value = value;
        return;
    }

    HashNode *hNode = malloc(sizeof(HashNode));
    hNode->next = NULL;
    hNode->key = key;
    hNode->value = value;

    hNode->next = hashTable->hashNode[hashValue].next;
    hashTable->hashNode[hashValue].next = hNode;
}

/**
 * @brief search for a value with given key in the target hash table.
 * @param hashTable the target hash table.
 * @param key the target key.
 * @return value of the given key in target hash table, return NOT_FOUND if key is not exist.
 */
int searchHashTable(HashTable *hashTable, int key) {
    size_t hashValue = hashOfKey(hashTable, key);

    if (hashTable->hashNode[hashValue].key == key) {
        return hashTable->hashNode[hashValue].value;
    }

    HashNode *sNode = hashTable->hashNode[hashValue].next;

    while (sNode != NULL) {
        if (sNode->key == key) {
            return sNode->value;
        }
        sNode = sNode->next;
    }

    return NOT_FOUND;
}

/**
 * @brief delete node from the target hash table with given key.
 * @param hashTable the target hash table.
 * @param key the target key.
 */
void deleteHashNode(HashTable *hashTable, int key) {
    size_t hashValue = hashOfKey(hashTable, key);

    if (hashTable->hashNode[hashValue].key == key) {
        hashTable->hashNode[hashValue].key = NO_KEY;
    }

    HashNode *currentNode = hashTable->hashNode[hashValue].next;
    HashNode *previousNode = &(hashTable->hashNode[hashValue]);

    while (currentNode != NULL) {
        if (currentNode->key == key) {
            previousNode->next = currentNode->next;
            currentNode->key = NO_KEY;
            free(currentNode);
            return;
        }
        previousNode = currentNode;
        currentNode = currentNode->next;
    }

}

/**
 * @brief destroy the hash table and free the memory spaces.
 * @param hashTable the target hash table.
 */
void destroyHashTable(HashTable *hashTable) {
    HashNode *currentNode, *previousNode;
    size_t i = 0;
    for ( i = 0; i < hashTable->size; ++i) {
        currentNode = hashTable->hashNode[i].next;
        while (currentNode != NULL) {
            previousNode = currentNode;
            currentNode = currentNode->next;
            free(previousNode);
        }
    }

    free(hashTable->hashNode);
    free(hashTable);
}

/**
 * @brief Replace the original key to a new one in target hash table.
 * @param hashTable the target hash table.
 * @param originalKey the original key for replacing.
 * @param newKey the new key for new value.
 * @param newValue the new value refer to the new key.
 */
void replaceHashNode(HashTable *hashTable, int originalKey, int newKey, int newValue) {
    deleteHashNode(hashTable, originalKey);
    insertHashNode(hashTable, newKey, newValue);
}

/**
 * @brief Search for the hash node with given key in the target hash table.
 * @param hashTable the target hash table.
 * @param key the key of the target hash node.
 * @return hash node of the given key in target hash table, return NULL if key is not exist.
 */
HashNode *getHashNode(HashTable *hashTable, int key) {
    size_t hashValue = hashOfKey(hashTable, key);

    if (hashTable->hashNode[hashValue].key == key) {
        return &(hashTable->hashNode[hashValue]);
    }

    HashNode *sNode = hashTable->hashNode[hashValue].next;

    while (sNode != NULL) {
        if (sNode->key == key) {
            return sNode;
        }
        sNode = sNode->next;
    }

    return NULL;

}

/**
 * @brief create an empty free buffer list.
 * @return an empty free list.
 */
FreeList *createFreeList() {
    FreeList *list;
    list = malloc(sizeof(FreeList));
    list->head = NULL;
    return list;
}

/**
 * @brief append new node by id to the end of the target free list
 * @param list the target free list to append
 * @param buff_id the target free buffer id
 */
void appendById(FreeList *list, int buff_id) {
    if (list == NULL) {
        printf("Create a List First");
        return;
    }

    FreeListNode *node = malloc(sizeof(FreeListNode));
    node->buff_id = buff_id;
    node->next = NULL;

    if (list->head == NULL) {
        list->head = node;
        return;
    }

    FreeListNode *currentNode;
    currentNode = list->head;

    while (currentNode->next) {
        currentNode = currentNode->next;
    }

    currentNode->next = node;

}

/**
 * @brief get the first node of the target free list.
 * @param list the target free list.
 * @return the first node of the target free list.
 */
FreeListNode *getFreeNode(FreeList *list) {
    if (list == NULL) {
        printf("Create a List First");
        return NULL;
    }

    if (list->head == NULL) {

        return NULL;
    }

    FreeListNode *node;
    node = list->head;

    list->head = list->head->next;

    return node;


}

/**
 * @brief destroy the free list and free the memory spaces.
 * @param list the target free list.
 */
void destroyFreeList(FreeList *list) {

    FreeListNode *node = list->head;
    FreeListNode *previousNode;
    while (node) {
        previousNode = node;
        node = node->next;
        free(previousNode);
    }
    free(list);
}

/**
 * @brief append new node to the end of the target free list
 * @param list the target free list to append
 * @param node the target node to append
 */
void appendByNode(FreeList *list, FreeListNode *node) {
    node->next = NULL;
    if (list->head == NULL) {
        list->head = node;
        return;
    }
    FreeListNode *currentNode;
    currentNode = list->head;
    while (currentNode->next) {
        currentNode = currentNode->next;
    }
    currentNode->next = node;
}

/**
 * @brief get the first node of the target node list.
 * @param list the target node list.
 * @return the first node of the target node list.
 */
ListNode *getListHead(List *list) {
    return list->head;
}

/**
 * @brief shift the target node to the end of the target list
 * @param list the target node list.
 * @param shiftNode the target node to shift.
 */
void shiftToEnd(List *list, ListNode *shiftNode) {
    if (list == NULL)
        return;

    ListNode *node = (list->head);
    ListNode *previousNode = NULL;

    if (node == NULL || shiftNode == NULL)
        return;

    while (node) {
        if (node->buff_id != shiftNode->buff_id) {
            previousNode = node;
            node = node->next;
            continue;
        }
        break;
    }

    if (node == NULL) {
        return;
    }

    if (node == list->head)
        list->head = node->next;
    else {
        previousNode->next = node->next;
    }
    appendByNode(list, shiftNode);
}

/**
 * @brief shift the target node to the end of the target list by buffer id
 * @param list the target node list.
 * @param buffId the target node buffer id to shift.
 */
void shiftNodeById(List *list, int buffId) {

    if (list == NULL)
        return;

    ListNode *node = list->head;
    ListNode *previousNode = NULL;

    if (node == NULL)
        return;

    while (node) {
        if (node->buff_id != buffId) {
            previousNode = node;
            node = node->next;
            continue;
        }
        break;
    }
    if (node == NULL)
        return;
    if (node == list->head)
        list->head = node->next;
    else
        previousNode->next = node->next;

    appendByNode(list, node);

}

/**
 * @brief creates a new buffer pool with numPages page frames using the page replacement strategy strategy.
 * @param bm stores information about a buffer pool.
 * @param pageFileName the page file name of pages to cache.
 * @param numPages number of the pages to cache.
 * @param strategy the replacement strategy of buffer pool.
 * @param startData pass parameters for the page replacement strategy.
 * @return RC_OK if init buffer successfully, RC_FILE_NOT_FOUND if file is not found.
 */
RC initBufferPool(BM_BufferPool *const bm, const char *const pageFileName, const int numPages,
                  ReplacementStrategy strategy, void *startData) {
    bm->isOpen = FALSE;
    SM_FileHandle *fHandle = malloc(sizeof(SM_FileHandle));
    char *fileName = malloc((strlen(pageFileName) + 1) * sizeof(char));
    strcpy(fileName, pageFileName);
    RC rc = openPageFile(fileName, fHandle);

    if (rc != RC_OK) {
        free(fHandle);
        return RC_FILE_NOT_FOUND;
    }

    bm->strategy = strategy;
    bm->numPages = numPages;
    bm->pageFile = fileName;
    bm->isOpen = TRUE;
    bm->mgmtData = malloc(sizeof(BM_MgmtData));
    bm->mgmtData->fHandle = fHandle;
    bm->mgmtData->buffPoolAddr = malloc(PAGE_SIZE * numPages * sizeof(char));
    memset(bm->mgmtData->buffPoolAddr, '\0', PAGE_SIZE * numPages * sizeof(char));
    bm->mgmtData->buffPoolHeaders = malloc(numPages * sizeof(BufferHeader));
    memset(bm->mgmtData->buffPoolHeaders, '\0', numPages * sizeof(BufferHeader));
    bm->mgmtData->buffTable = createHashTable((size_t) pow(2, (double) (log(2 * numPages) / log(2))));
    bm->mgmtData->fixCount = malloc(sizeof(int) * numPages);
    memset(bm->mgmtData->fixCount, 0, sizeof(int) * numPages);
    bm->mgmtData->freeBuffList = createFreeList();

    if (strategy == RS_FIFO || strategy == RS_LRU || strategy == RS_LRU_K) {
        bm->mgmtData->strategyData = createFreeList();
    }

    bm->mgmtData->buffStats.num_buff_hits = 0;
    bm->mgmtData->buffStats.num_reads_disk = 0;
    bm->mgmtData->buffStats.num_writes_disk = 0;

    unsigned int i;
    for (i = 0; i < numPages; ++i) {
        bm->mgmtData->buffPoolHeaders[i].buff_id = i;
        bm->mgmtData->buffPoolHeaders[i].pageNumber = NO_PAGE;
        bm->mgmtData->buffPoolHeaders[i].dirtyPage = FALSE;
        bm->mgmtData->buffPoolHeaders[i].pinned = FALSE;
        bm->mgmtData->fixCount[i] = 0;
        appendById(bm->mgmtData->freeBuffList, i);
    }

    return RC_OK;
}

/**
 * @brief destroys a buffer pool and free up all resources associated with buffer pool.
 * @param bm stores information about a buffer pool.
 * @return RC_OK if shutdown successfully else RC_FILE_NOT_FOUND.
 */
RC shutdownBufferPool(BM_BufferPool *const bm) {
    if (!bm->isOpen)
        return RC_BUFF_SHUT_FAILED;
    // force flush all dirty pages to disk
    RC rc = forceFlushPool(bm);
    if (rc != RC_OK) {
        return RC_FLUSH_FAILED;
    }

    // check the pinned pages, throw error if there is any page is pinned.
    size_t i;
    for (i = 0; i < bm->numPages; i++) {
        if (bm->mgmtData->buffPoolHeaders[i].pinned) {
            return RC_BUFF_SHUT_FAILED;
        }
    }

    // close page file
    rc = closePageFile(bm->mgmtData->fHandle);
    if (rc != RC_OK) {
        return rc;
    }

    // free all allocated data
    free(bm->pageFile);
    free(bm->mgmtData->buffPoolAddr);
    free(bm->mgmtData->buffPoolHeaders);
    free(bm->mgmtData->fixCount);
    free(bm->mgmtData->fHandle);
    destroyHashTable(bm->mgmtData->buffTable);
    destroyFreeList(bm->mgmtData->freeBuffList);
    destroyFreeList(bm->mgmtData->strategyData);
    free(bm->mgmtData);
    return RC_OK;
}


/**
 * @brief causes all dirty pages (with fix count 0) from the buffer pool to be written to disk.
 * @param bm stores information about a buffer pool.
 * @return RC_OK if flush successfully else RC_FLUSH_FAILED.
 */
RC forceFlushPool(BM_BufferPool *const bm) {
    if (!bm->isOpen)
        return RC_FLUSH_FAILED;
    size_t i;
    BM_PageHandle *pHandle = malloc(sizeof(BM_PageHandle));
    pHandle->data = NULL;
    RC rc;
    // traversal all the buffering pages, flush if there is any dirty page.
    for (i = 0; i < bm->numPages; ++i) {
        if (bm->mgmtData->buffPoolHeaders[i].dirtyPage) {
            pHandle->pageNum = bm->mgmtData->buffPoolHeaders[i].pageNumber;
            assert(bm->mgmtData->buffPoolHeaders[i].pageNumber >= 0);
            rc = forcePage(bm, pHandle);

            if (rc < 0) {
                free(pHandle);
                return RC_FLUSH_FAILED;
            }
        }
    }
    free(pHandle);
    return RC_OK;
}

/**
 * @brief pins the page with page number pageNum.
 * @param bm stores information about a buffer pool.
 * @param page stores information about a page.
 * @param pageNum the page number need to pin.
 * @return RC_OK if pin successfully else code defined in dberror.h.
 */
RC pinPage(BM_BufferPool *const bm, BM_PageHandle *const page, const PageNumber pageNum) {

    if (pageNum < 0 || !bm->isOpen)
        return RC_PIN_FAILED;

    HashTable *hTable = bm->mgmtData->buffTable;

    // search for the page in the hash table
    int buffId = searchHashTable(hTable, pageNum);
    BufferHeader *buffHead;

    char *buffPool;
    buffPool = bm->mgmtData->buffPoolAddr;
    ListNode *node;

    // buffering the pages, replace the page by the target replacement strategy if needed.
    if (buffId < 0) {
        // check for the available buffer slot
        node = getFreeNode(bm->mgmtData->freeBuffList);

        if (node == NULL) {
            // replace the page by the target replacement strategy if needed
            if (bm->strategy == RS_FIFO || bm->strategy == RS_LRU || bm->strategy == RS_LRU_K) {
                node = getListHead(bm->mgmtData->strategyData);
                if (node == NULL)
                    return RC_PIN_FAILED;

                bool shifted = FALSE;
                while (node) {
                    buffId = node->buff_id;
                    if (bm->mgmtData->buffPoolHeaders[buffId].pinned || bm->mgmtData->fixCount[buffId] > 0) {
                        node = node->next;
                    } else {
                        shiftToEnd(bm->mgmtData->strategyData, node);
                        shifted = TRUE;
                        break;
                    }
                }
                if (!shifted)
                    return RC_PIN_FAILED;
            }

            // flush if it is dirty buffer
            buffHead = &(bm->mgmtData->buffPoolHeaders[buffId]);
            BM_PageHandle *pHand = malloc(sizeof(BM_PageHandle));

            pHand->data = NULL;
            pHand->pageNum = buffHead->pageNumber;
            if (buffHead->dirtyPage)
                forcePage(bm, pHand);

            free(pHand);

        } else {
            if (bm->strategy == RS_FIFO || bm->strategy == RS_LRU || bm->strategy == RS_LRU_K)
                appendByNode(bm->mgmtData->strategyData, node);

            buffId = node->buff_id;
        }

        // ensure capacity before reading the page
        RC rc;
        rc = ensureCapacity(pageNum + 1, bm->mgmtData->fHandle);
        if (rc != RC_OK) {
            return rc;
        }

        // read page from disk to buffer
        rc = readBlock(pageNum, bm->mgmtData->fHandle, &buffPool[buffId * PAGE_SIZE]);
        if (rc != RC_OK) {
            return rc;
        }

        // replace the mapping in the hash table
        replaceHashNode(bm->mgmtData->buffTable, bm->mgmtData->buffPoolHeaders[buffId].pageNumber, pageNum, buffId);

        bm->mgmtData->buffStats.num_reads_disk += 1;

    } else {
        if (bm->strategy == RS_LRU || (bm->strategy == RS_LRU_K)) {
            printf("bm->mgmtData->fixCount[buffId] = %i", bm->mgmtData->fixCount[buffId]);
            shiftNodeById(bm->mgmtData->strategyData, buffId);
        }
        bm->mgmtData->buffStats.num_buff_hits += 1;
    }


    buffHead = &(bm->mgmtData->buffPoolHeaders[buffId]);
    buffHead->pinned = TRUE;
    buffHead->pageNumber = pageNum;
    bm->mgmtData->fixCount[buffId] += 1;

    assert(bm->mgmtData->buffPoolHeaders[buffId].pageNumber >= 0);
    page->pageNum = buffHead->pageNumber;
    page->data = &(buffPool[buffId * PAGE_SIZE]);

    return RC_OK;
}

/**
 * @brief unpins the page page.
 * @param bm stores information about a buffer pool.
 * @param page stores information about a page.
 * @return RC_OK if unpin successfully else RC_UNPIN_FAILED.
 */
RC unpinPage(BM_BufferPool *const bm, BM_PageHandle *const page) {
    HashTable *hTable = bm->mgmtData->buffTable;
    int buffId = searchHashTable(hTable, page->pageNum);

    if (buffId < 0) {
        printf("cannot unpin page which is not in buffer, please check.\n");
        return RC_UNPIN_FAILED;
    }

    bm->mgmtData->buffPoolHeaders[buffId].pinned = FALSE;
    assert(bm->mgmtData->buffPoolHeaders[buffId].pageNumber >= 0);
    if (bm->mgmtData->fixCount[buffId] > 0) {
        bm->mgmtData->fixCount[buffId] -= 1;
    }
    return RC_OK;
}

/**
 * @brief marks a page as dirty.
 * @param bm stores information about a buffer pool.
 * @param page stores information about a page.
 * @return RC_OK if mark successfully else RC_DIRTY_FAILED.
 */
RC markDirty(BM_BufferPool *const bm, BM_PageHandle *const page) {

    if (page == NULL) {
        return RC_DIRTY_FAILED;
    }

    HashTable *hTable = bm->mgmtData->buffTable;

    int buffId = searchHashTable(hTable, page->pageNum);
    if (buffId < 0) {
        printf("Trying to mark a page which is not in buffer as dirty, please check.\n");
        return RC_DIRTY_FAILED;
    }
    bm->mgmtData->buffPoolHeaders[buffId].dirtyPage = TRUE;

    return RC_OK;
}

/**
 * @brief write the current content of the page back to the page file on disk.
 * @param bm stores information about a buffer pool.
 * @param page stores information about a page.
 * @return RC_OK if written successfully else code defined in dberror.h.
 */
RC forcePage(BM_BufferPool *const bm, BM_PageHandle *const page) {
    if (page == NULL || bm->mgmtData->fHandle == NULL) {
        return RC_FLUSH_FAILED;
    }

    int buff_id = searchHashTable(bm->mgmtData->buffTable, page->pageNum);
    if (buff_id < 0) {
        printf("Cannot flush a page is not in buffer, please check.\n");
        return RC_FLUSH_FAILED;
    }

    SM_PageHandle data = malloc(sizeof(char) * PAGE_SIZE);
    memset(data, '\0', sizeof(char) * PAGE_SIZE);
    memcpy(data, &(bm->mgmtData->buffPoolAddr[buff_id * PAGE_SIZE]), PAGE_SIZE);

    RC rc = writeBlock(page->pageNum, bm->mgmtData->fHandle, data);

    free(data);
    if (rc != RC_OK) {
        printf("Cannot write page to disk.");
        return RC_FLUSH_FAILED;
    }

    bm->mgmtData->buffStats.num_writes_disk += 1;
    if (bm->mgmtData->fixCount[buff_id] == 0) {
        bm->mgmtData->buffPoolHeaders[buff_id].dirtyPage = FALSE;
    }

    return RC_OK;
}

/**
 * @brief get an array of PageNumbers (of size numPages).
 * @param bm stores information about a buffer pool.
 * @param page stores information about a page.
 * @return an array of PageNumbers (of size numPages) where
 * the ith element is the number of the page stored in the ith page frame.
 * An empty page frame is represented using the constant NO PAGE.
 */
PageNumber *getFrameContents(BM_BufferPool *const bm) {
    PageNumber *pageNbrArr = malloc(sizeof(PageNumber) * bm->numPages);

    // if (bm->numPages < 1)
    //     return NO_PAGE;
    int i=0;
    for ( i = 0; i < bm->numPages; i++) {
        pageNbrArr[i] = bm->mgmtData->buffPoolHeaders[i].pageNumber;

    }
    return pageNbrArr;
}

/**
 * @brief get an array of bools (of size numPages).
 * @param bm stores information about a buffer pool.
 * @param page stores information about a page.
 * @return an array of bools (of size numPages) where
 * the ith element is TRUE if the page stored in the ith page frame is dirty.
 */
bool *getDirtyFlags(BM_BufferPool *const bm) {
    bool *flagArr = malloc(sizeof(bool) * bm->numPages);
    int i=0;
    for ( i = 0; i < bm->numPages; ++i) {
        flagArr[i] = bm->mgmtData->buffPoolHeaders[i].dirtyPage;
    }
    return flagArr;
}

/**
 * @brief get an array of ints (of size numPages).
 * @param bm stores information about a buffer pool.
 * @param page stores information about a page.
 * @return an array of ints (of size numPages) where
 * the ith element is the fix count of the page stored in the ith page frame.
 * Return 0 for empty page frames.
 */
int *getFixCounts(BM_BufferPool *const bm) {
    int *arr = malloc(sizeof(int) * bm->numPages);
    if (bm->numPages < 1)
        return 0;
    int i=0;
    for ( i = 0; i < bm->numPages; ++i) {
        arr[i] = bm->mgmtData->fixCount[i];
    }
    return arr;
}

/**
 * @brief get the number of pages that have been read from disk
 * since a buffer pool has been initialized.
 * @param bm stores information about a buffer pool.
 * @return the number of pages that have been read from disk
 * since a buffer pool has been initialized
 */
int getNumReadIO(BM_BufferPool *const bm) {
    return bm->mgmtData->buffStats.num_reads_disk;
}

/**
 * @brief get the number of pages written to the page file
 * since the buffer pool has been initialized.
 * @param bm stores information about a buffer pool.
 * @return the number of pages written to the page file
 * since the buffer pool has been initialized.
 */
int getNumWriteIO(BM_BufferPool *const bm) {
    return bm->mgmtData->buffStats.num_writes_disk;
}
