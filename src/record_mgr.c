#include "record_mgr.h"
#include "buffer_mgr.h"
#include "storage_mgr.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* =========================================================================
 * Record Manager - tables, records, and scans on top of the buffer manager
 *
 * Table file layout:
 *   - page 0    : serialized schema + tuple count (see writeTableSchema).
 *   - page 1..N : data pages.
 *
 * Data page layout (marker-based, no per-page record counter): each slot
 * occupies (recordSize + 2) bytes:
 *       [0]      marker byte : '+' occupied, '-' tombstone (deleted),
 *                              '\0' never used
 *       [1..]    the record bytes
 *   offset of slot s = s * (recordSize + 2)
 *
 * The tuple count (numTuples) lives in memory on RM_TableData, is persisted
 * to page 0 by closeTable, and restored by openTable.
 *
 * Scans pin one page at a time and bound the walk by the number of pages
 * captured at startScan (see next()).
 * ========================================================================= */

// Data structure to store information about an ongoing scan
typedef struct RM_ScanInfo
{
    int curPage;     // Current page being scanned
    int curSlot;     // Current slot being scanned
    Expr *condition; // Scan condition
    int totalPages;  // Number of pages in the table file at scan start
} RM_ScanInfo;

const int NUM_PAGES = 10;

// Helper function to calculate the total number of records that can be stored on a page
int getRecordsPerPage(Schema *schema)
{
    int recordSize = getRecordSize(schema);
    int pageSize = PAGE_SIZE;
    int numSlots = pageSize / (recordSize + 2); // +1 for the marker
    return numSlots;
}

RC initRecordManager(void *mgmtData)
{
    // Nothing to initialize in this record manager
    return RC_OK;
}

RC shutdownRecordManager()
{
    // Nothing to shutdown in this record manager
    return RC_OK;
}

/* Write the schema (and initial tuple count) of a table to its page file.
 *
 * Binary layout of the schema pages (starting at page 0):
 *   [0..3]   numPagesOfSchema (int)
 *   [4..7]   numTuples        (int)
 *   [8..11]  numAttr          (int)
 *   [12..15] keySize          (int)
 *   per attribute (numAttr):  attrNameOffset(int), nameLen(int),
 *                             dataType(DataType), typeLength(int)
 *   keyAttrs[keySize]         (int each)
 *   attribute name strings at their attrNameOffset (absolute in the buffer)
 */
static RC
writeTableSchema(SM_FileHandle *fh, Schema *schema, int numTuples)
{
    int i;

    int sizeSchema = sizeof(int) * (4 + schema->numAttr * 3 + schema->keySize)
                     + sizeof(DataType) * schema->numAttr;
    for (i = 0; i < schema->numAttr; i++) {
        sizeSchema += strlen(schema->attrNames[i]) + 1;
    }

    int numPagesOfSchema = (sizeSchema - 1) / PAGE_SIZE + 1;
    char *buffer = (char *) calloc(numPagesOfSchema, PAGE_SIZE);
    if (buffer == NULL) {
        return RC_RM_MEM_ALLOC_FAILED;
    }

    int attrNameOffset = sizeSchema;
    int offset = 0;

    memcpy(buffer + offset, &numPagesOfSchema, sizeof(int)); offset += sizeof(int);
    memcpy(buffer + offset, &numTuples,         sizeof(int)); offset += sizeof(int);
    memcpy(buffer + offset, &schema->numAttr,   sizeof(int)); offset += sizeof(int);
    memcpy(buffer + offset, &schema->keySize,   sizeof(int)); offset += sizeof(int);

    for (i = 0; i < schema->numAttr; i++) {
        int nameLen = strlen(schema->attrNames[i]) + 1;
        memcpy(buffer + offset, &attrNameOffset, sizeof(int)); offset += sizeof(int);
        memcpy(buffer + offset, &nameLen,        sizeof(int)); offset += sizeof(int);
        memcpy(buffer + offset, &schema->dataTypes[i],  sizeof(DataType)); offset += sizeof(DataType);
        memcpy(buffer + offset, &schema->typeLength[i], sizeof(int));       offset += sizeof(int);
        memcpy(buffer + attrNameOffset, schema->attrNames[i], nameLen);
        attrNameOffset += nameLen;
    }

    for (i = 0; i < schema->keySize; i++) {
        memcpy(buffer + offset, &schema->keyAttrs[i], sizeof(int)); offset += sizeof(int);
    }

    for (i = 0; i < numPagesOfSchema; i++) {
        if (writeBlock(i, fh, buffer + i * PAGE_SIZE) != RC_OK) {
            free(buffer);
            return RC_WRITE_FAILED;
        }
    }

    free(buffer);
    return RC_OK;
}

RC createTable(char *name, Schema *schema)
{
    if (schema == NULL) {
        return RC_RM_INVALID_SCHEMA_DATA;
    }

    SM_FileHandle fileHandle;

    // Create a new page file
    if (createPageFile(name) != RC_OK) {
        return RC_WRITE_FAILED;
    }

    if (openPageFile(name, &fileHandle) != RC_OK) {
        return RC_FILE_NOT_FOUND;
    }

    // Write the table schema (and an initial tuple count of 0) to page 0
    if (writeTableSchema(&fileHandle, schema, 0) != RC_OK) {
        closePageFile(&fileHandle);
        return RC_WRITE_FAILED;
    }

    if (closePageFile(&fileHandle) != RC_OK) {
        return RC_FILE_HANDLE_NOT_INIT;
    }

    return RC_OK;
}

RC openTable(RM_TableData *rel, char *name)
{
    // Initialize buffer pool for the table
    rel->mgmtData = MAKE_POOL();
    if (initBufferPool(rel->mgmtData, name, NUM_PAGES, RS_FIFO, NULL) != RC_OK)
    {
        return RC_BM_POOL_INIT_FAILED;
    }

    // Read the schema metadata from the first page of the file
    BM_PageHandle pageHandle;
    if (pinPage(rel->mgmtData, &pageHandle, 0) != RC_OK)
    {
        return RC_RM_BUFFER_PIN_FAILED;
    }

    int numPagesOfSchema, numTuples;
    memcpy(&numPagesOfSchema, pageHandle.data,     sizeof(int));
    memcpy(&numTuples,        pageHandle.data + 4, sizeof(int));
    unpinPage(rel->mgmtData, &pageHandle);

    if (numPagesOfSchema <= 0 || numPagesOfSchema > 1024) {
        shutdownBufferPool(rel->mgmtData);
        return RC_RM_INVALID_SCHEMA_DATA;
    }

    // Read all schema pages into a contiguous buffer
    char *buffer = (char *) malloc(numPagesOfSchema * PAGE_SIZE);
    if (buffer == NULL) {
        shutdownBufferPool(rel->mgmtData);
        return RC_RM_MEM_ALLOC_FAILED;
    }

    int i;
    for (i = 0; i < numPagesOfSchema; i++) {
        if (pinPage(rel->mgmtData, &pageHandle, i) != RC_OK) {
            free(buffer);
            shutdownBufferPool(rel->mgmtData);
            return RC_RM_BUFFER_PIN_FAILED;
        }
        memcpy(buffer + i * PAGE_SIZE, pageHandle.data, PAGE_SIZE);
        unpinPage(rel->mgmtData, &pageHandle);
    }

    // Rebuild the schema from the binary buffer
    int offset = 8; /* skip numPagesOfSchema and numTuples */
    int numAttr, keySize;
    memcpy(&numAttr, buffer + offset, sizeof(int)); offset += sizeof(int);
    memcpy(&keySize, buffer + offset, sizeof(int)); offset += sizeof(int);

    char **names = (char **) malloc(sizeof(char *) * numAttr);
    DataType *dataTypes = (DataType *) malloc(sizeof(DataType) * numAttr);
    int *typeLength = (int *) malloc(sizeof(int) * numAttr);
    int *keys = (int *) malloc(sizeof(int) * (keySize > 0 ? keySize : 1));

    for (i = 0; i < numAttr; i++) {
        int attrNameOffset, nameLen;
        memcpy(&attrNameOffset, buffer + offset, sizeof(int)); offset += sizeof(int);
        memcpy(&nameLen,        buffer + offset, sizeof(int)); offset += sizeof(int);
        memcpy(&dataTypes[i],  buffer + offset, sizeof(DataType)); offset += sizeof(DataType);
        memcpy(&typeLength[i], buffer + offset, sizeof(int));       offset += sizeof(int);

        names[i] = (char *) malloc(nameLen);
        memcpy(names[i], buffer + attrNameOffset, nameLen);
    }
    for (i = 0; i < keySize; i++) {
        memcpy(&keys[i], buffer + offset, sizeof(int)); offset += sizeof(int);
    }
    free(buffer);

    // createSchema copies the values and keeps the attribute name strings
    rel->schema = createSchema(numAttr, names, dataTypes, typeLength, keySize, keys);
    free(names);
    free(dataTypes);
    free(typeLength);
    free(keys);

    rel->name = name;
    rel->numTuples = numTuples;

    return RC_OK;
}

RC closeTable(RM_TableData *rel)
{
    // Persist the current tuple count back into the schema page (offset 4)
    BM_PageHandle pageHandle;
    if (pinPage(rel->mgmtData, &pageHandle, 0) != RC_OK)
    {
        return RC_RM_BUFFER_PIN_FAILED;
    }

    memcpy(pageHandle.data + 4, &rel->numTuples, sizeof(int));
    if (markDirty(rel->mgmtData, &pageHandle) != RC_OK)
    {
        return RC_RM_MARK_DIRTY_FAILED;
    }
    if (unpinPage(rel->mgmtData, &pageHandle) != RC_OK)
    {
        return RC_RM_BUFFER_UNPIN_FAILED;
    }

    // Force writing any dirty pages to disk before closing the table
    if (forceFlushPool(rel->mgmtData) != RC_OK)
    {
        return RC_WRITE_FAILED;
    }

    // Shutdown the buffer pool and free resources
    if (shutdownBufferPool(rel->mgmtData) != RC_OK)
    {
        return RC_RM_BUFFER_POOL_SHUTDOWN_FAILED;
    }

    // Free schema and table metadata
    freeSchema(rel->schema);
    free(rel->mgmtData);
    rel->schema = NULL;
    rel->mgmtData = NULL;

    return RC_OK;
}

RC deleteTable(char *name)
{
    // Delete the page file associated with the table
    if (destroyPageFile(name) != RC_OK)
    {
        return RC_FILE_NOT_FOUND;
    }

    return RC_OK;
}

int getNumTuples(RM_TableData *rel)
{
    return rel->numTuples;
}

RC insertRecord(RM_TableData *rel, Record *record)
{
    // Get the table metadata
    // TableInfo *tableInfo = (TableInfo *)rel->mgmtData;

    // Get the record size
    int recordSize = getRecordSize(rel->schema);

    // Calculate the total number of records that can be stored on a page
    int numSlots = getRecordsPerPage(rel->schema);

    // Find a page with available slots to insert the record
    int curPageNum = 1;
    bool foundPage = false;

    while (!foundPage)
    {
        // Pin the current page
        BM_PageHandle pageHandle;
        if (pinPage(rel->mgmtData, &pageHandle, curPageNum) != RC_OK)
        {
            return RC_RM_BUFFER_PIN_FAILED;
        }

        // Find the first free slot on this page (marker != '+'): either a never
        // used slot ('\0') or a tombstone left by deleteRecord ('-'). Reusing
        // tombstone holes matters: inserting at the "count of live records"
        // index would overwrite a record that sits after a deleted one.
        int slotNum = -1;
        for (int i = 0; i < numSlots; i++)
        {
            int offset = i * (recordSize + 2);
            if (pageHandle.data[offset] != '+')
            {
                slotNum = i;
                break;
            }
        }

        // Check if there is space for another record
        if (slotNum >= 0)
        {
            // Insert the record in the free slot
            int offset = slotNum * (recordSize + 2);
            char *data = record->data;
            memcpy(pageHandle.data + offset + 1, data, recordSize);
            pageHandle.data[offset] = '+';
            record->id.page = curPageNum;
            record->id.slot = slotNum;

            // Mark the page as dirty
            if (markDirty(rel->mgmtData, &pageHandle) != RC_OK)
            {
                return RC_RM_MARK_DIRTY_FAILED;
            }

            // Update the number of tuples in the table
            rel->numTuples++;

            // Unpin the page
            if (unpinPage(rel->mgmtData, &pageHandle) != RC_OK)
            {
                return RC_RM_BUFFER_UNPIN_FAILED;
            }

            foundPage = true;
        }
        else
        {
            // No space on the current page, unpin and move to the next page
            if (unpinPage(rel->mgmtData, &pageHandle) != RC_OK)
            {
                return RC_RM_BUFFER_UNPIN_FAILED;
            }

            curPageNum++;
        }
    }

    return RC_OK;
}

RC deleteRecord(RM_TableData *rel, RID id)
{
    // Get the table metadata
    // TableInfo *tableInfo = (TableInfo *)rel->mgmtData;

    // Get the page size and record size
    int pageSize = PAGE_SIZE;
    int recordSize = getRecordSize(rel->schema);

    // Calculate the total number of records that can be stored on a page
    int numSlots = getRecordsPerPage(rel->schema);

    // Check if the RID is valid
    // if (id.page < 1 || id.page > rel->numTuples || id.slot < 0 || id.slot >= numSlots)
    // {
    //     return RC_RM_INVALID_RID;
    // }

    // Pin the page containing the record to delete
    BM_PageHandle pageHandle;
    if (pinPage(rel->mgmtData, &pageHandle, id.page) != RC_OK)
    {
        return RC_RM_BUFFER_PIN_FAILED;
    }

    // Calculate the offset of the record to delete
    int offset = id.slot * (recordSize + 2);

    // Check if the slot is empty (no record to delete)
    char marker = pageHandle.data[offset];
    if (marker != '+')
    {
        // Release the pin before returning the error
        unpinPage(rel->mgmtData, &pageHandle);
        return RC_RM_INVALID_RID;
    }

    // Mark the slot as empty
    pageHandle.data[offset] = '-';

    // Mark the page as dirty
    if (markDirty(rel->mgmtData, &pageHandle) != RC_OK)
    {
        return RC_RM_MARK_DIRTY_FAILED;
    }

    // Update the number of tuples in the table
    rel->numTuples--;

    // Unpin the page
    if (unpinPage(rel->mgmtData, &pageHandle) != RC_OK)
    {
        return RC_RM_BUFFER_UNPIN_FAILED;
    }

    return RC_OK;
}

RC updateRecord(RM_TableData *rel, Record *record)
{
    // Get the table metadata
    // TableInfo *tableInfo = (TableInfo *)rel->mgmtData;

    // Get the page size and record size
    int pageSize = PAGE_SIZE;
    int recordSize = getRecordSize(rel->schema);

    // Calculate the total number of records that can be stored on a page
    int numSlots = getRecordsPerPage(rel->schema);

    // Check if the RID is valid
    // if (record->id.page < 1 || record->id.page > rel->mgmtData->numPages ||
    //     record->id.slot < 0 || record->id.slot >= numSlots) {
    //     return RC_RM_INVALID_RID;
    // }

    // Pin the page containing the record to update
    BM_PageHandle pageHandle;
    if (pinPage(rel->mgmtData, &pageHandle, record->id.page) != RC_OK)
    {
        return RC_RM_BUFFER_PIN_FAILED;
    }

    // Calculate the offset of the record to update
    int offset = record->id.slot * (recordSize + 2);

    // Check if the slot is empty (no record to update)
    char marker = pageHandle.data[offset];
    if (marker != '+')
    {
        // Release the pin before returning the error
        unpinPage(rel->mgmtData, &pageHandle);
        return RC_RM_INVALID_RID;
    }

    // Update the record on the page
    char *data = record->data;
    memcpy(pageHandle.data + offset + 1, data, recordSize);

    // Mark the page as dirty
    if (markDirty(rel->mgmtData, &pageHandle) != RC_OK)
    {
        return RC_RM_MARK_DIRTY_FAILED;
    }

    // Unpin the page
    if (unpinPage(rel->mgmtData, &pageHandle) != RC_OK)
    {
        return RC_RM_BUFFER_UNPIN_FAILED;
    }

    return RC_OK;
}

RC getRecord(RM_TableData *rel, RID id, Record *record)
{
    // Get the table metadata
    // TableInfo *tableInfo = (TableInfo *)rel->mgmtData;

    // Get the page size and record size
    int pageSize = PAGE_SIZE;
    int recordSize = getRecordSize(rel->schema);

    // Calculate the total number of records that can be stored on a page
    // int numSlots = tableInfo->numSlots;

    // Check if the RID is valid
    // if (id.page < 1 || id.page > tableInfo->bufferPool->numPages || id.slot < 0 || id.slot >= numSlots) {
    //     return RC_RM_INVALID_RID;
    // }

    // Pin the page containing the record to retrieve
    BM_PageHandle pageHandle;
    if (pinPage(rel->mgmtData, &pageHandle, id.page) != RC_OK)
    {
        return RC_RM_BUFFER_PIN_FAILED;
    }

    // Calculate the offset of the record to retrieve
    int offset = id.slot * (recordSize + 2);

    // Check if the slot is empty (no record to retrieve)
    char marker = pageHandle.data[offset];
    if (marker != '+')
    {
        // Release the pin before returning the error
        unpinPage(rel->mgmtData, &pageHandle);
        return RC_RM_INVALID_RID;
    }

    // Copy the record data to the output Record struct
    char *data = pageHandle.data + offset + 1;
    memcpy(record->data, data, recordSize);
    record->id = id;

    // Unpin the page
    if (unpinPage(rel->mgmtData, &pageHandle) != RC_OK)
    {
        return RC_RM_BUFFER_UNPIN_FAILED;
    }

    return RC_OK;
}

RC startScan(RM_TableData *rel, RM_ScanHandle *scan, Expr *cond)
{
    // Allocate memory for the RM_ScanInfo struct
    RM_ScanInfo *scanInfo = (RM_ScanInfo *)malloc(sizeof(RM_ScanInfo));
    if (scanInfo == NULL)
    {
        return RC_RM_MEM_ALLOC_FAILED;
    }

    // Initialize scan parameters
    scanInfo->curPage = 1;
    scanInfo->curSlot = -1;
    scanInfo->condition = cond;

    // Capture the table's real page count at scan start. This bound is fixed
    // here so the scan can't chase the file growing inside pinPage.
    scanInfo->totalPages = getTotalNumPages((BM_BufferPool *)rel->mgmtData);

    // Assign the scanInfo to the RM_ScanHandle
    scan->rel = rel;
    scan->mgmtData = scanInfo;

    return RC_OK;
}

RC next(RM_ScanHandle *scan, Record *record)
{
    // Get the scan info
    RM_ScanInfo *scanInfo = (RM_ScanInfo *)scan->mgmtData;
    // Get the record size
    int recordSize = getRecordSize(scan->rel->schema);
    // Get the number of slots per page
    int numSlots = getRecordsPerPage(scan->rel->schema);

    while (true)
    {
        // Check if we reached the end of the table. The bound is the page
        // count captured at scan start (>= because data starts at page 1, so
        // the last valid data page is totalPages - 1). This must NOT be a
        // live getTotalNumPages() call: pinPage grows the file when it reads
        // a page past the end, which would let the bound chase the file
        // forever.
        if (scanInfo->curPage >= scanInfo->totalPages) {
            return RC_RM_NO_MORE_TUPLES;
        }

        // Move to the next slot on the current page
        scanInfo->curSlot++;

        // Check if we reached the end of the page
        if (scanInfo->curSlot >= numSlots)
        {
            // Move to the next page
            scanInfo->curPage++;
            scanInfo->curSlot = 0;
            continue;
        }

        // Pin the current page
        BM_PageHandle pageHandle;
        if (pinPage(scan->rel->mgmtData, &pageHandle, scanInfo->curPage) != RC_OK)
        {
            return RC_RM_BUFFER_PIN_FAILED;
        }

        // Calculate the offset of the current record
        int offset = scanInfo->curSlot * (recordSize + 2);

        // Check if the slot is occupied
        char marker = pageHandle.data[offset];
        if (marker != '+')
        {
            // Unpin the page and continue to the next slot
            if (unpinPage(scan->rel->mgmtData, &pageHandle) != RC_OK)
            {
                return RC_RM_BUFFER_UNPIN_FAILED;
            }
            continue;
        }

        // Copy the record data to the output Record struct
        char *data = pageHandle.data + offset + 1;
        memcpy(record->data, data, recordSize);
        record->id.page = scanInfo->curPage;
        record->id.slot = scanInfo->curSlot;

        // Check if the scan condition is satisfied
        if (scanInfo->condition == NULL)
        {
            // No condition, return the current record
            // Unpin the page and return success
            if (unpinPage(scan->rel->mgmtData, &pageHandle) != RC_OK)
            {
                return RC_RM_BUFFER_UNPIN_FAILED;
            }
            return RC_OK;
        }
        else
        {
            // Evaluate the condition
            Value *result = NULL;
            if (evalExpr(record, scan->rel->schema, scanInfo->condition, &result) != RC_OK)
            {
                // Unpin the page and return error
                if (unpinPage(scan->rel->mgmtData, &pageHandle) != RC_OK)
                {
                    return RC_RM_BUFFER_UNPIN_FAILED;
                }
                return RC_RM_SCAN_CONDITION_EVAL_FAILED;
            }

            // Check if the condition is true
            if (result->v.boolV)
            {
                // Condition is true, return the current record
                // Unpin the page and return success
                freeVal(result);
                if (unpinPage(scan->rel->mgmtData, &pageHandle) != RC_OK)
                {
                    return RC_RM_BUFFER_UNPIN_FAILED;
                }
                return RC_OK;
            }

            // Condition is false, move to the next slot
            freeVal(result);
            if (unpinPage(scan->rel->mgmtData, &pageHandle) != RC_OK)
            {
                return RC_RM_BUFFER_UNPIN_FAILED;
            }
        }
    }
}

RC closeScan(RM_ScanHandle *scan)
{
    // Free the scanInfo struct
    free(scan->mgmtData);
    scan->mgmtData = NULL;

    return RC_OK;
}

int getRecordSize(Schema *schema)
{
    int recordSize = 0;
    for (int i = 0; i < schema->numAttr; i++)
    {
        switch (schema->dataTypes[i])
        {
        case DT_INT:
            recordSize += sizeof(int);
            break;
        case DT_FLOAT:
            recordSize += sizeof(float);
            break;
        case DT_BOOL:
            recordSize += sizeof(bool);
            break;
        case DT_STRING:
            recordSize += schema->typeLength[i];
            break;
        }
    }
    return recordSize;
}

Schema *createSchema(int numAttr, char **attrNames, const DataType *dataTypes,
                     const int *typeLength, int keySize, const int *keys)
{
    if (numAttr <= 0)
    {
        return NULL;
    }

    // Allocate memory for the schema struct
    Schema *schema = (Schema *)malloc(sizeof(Schema));
    if (schema == NULL)
    {
        return NULL;
    }

    schema->numAttr = numAttr;
    schema->keySize = keySize;

    // Allocate our own arrays; copy the values but keep the attribute name
    // string pointers (callers own those strings, freeSchema frees them).
    schema->attrNames = (char **)malloc(sizeof(char *) * numAttr);
    schema->dataTypes = (DataType *)malloc(sizeof(DataType) * numAttr);
    schema->typeLength = (int *)malloc(sizeof(int) * numAttr);
    schema->keyAttrs = (int *)malloc(sizeof(int) * (keySize > 0 ? keySize : 1));

    int i;
    for (i = 0; i < numAttr; i++)
    {
        schema->attrNames[i] = attrNames[i];
        schema->dataTypes[i] = dataTypes[i];
        schema->typeLength[i] = typeLength[i];
    }
    for (i = 0; i < keySize; i++)
    {
        schema->keyAttrs[i] = keys[i];
    }

    return schema;
}

RC freeSchema(Schema *schema)
{
    if (schema == NULL)
    {
        return RC_OK;
    }

    // Free attribute names and the schema itself
    int i;
    for (i = 0; i < schema->numAttr; i++)
    {
        if (schema->attrNames[i] != NULL)
        {
            free(schema->attrNames[i]);
        }
    }
    free(schema->attrNames);
    free(schema->dataTypes);
    free(schema->typeLength);
    free(schema->keyAttrs);
    free(schema);

    return RC_OK;
}

RC createRecord(Record **record, Schema *schema)
{
    // Allocate memory for the Record struct
    *record = (Record *)malloc(sizeof(Record));
    if (*record == NULL)
    {
        return RC_RM_MEM_ALLOC_FAILED;
    }

    // Allocate memory for the record data
    int recordSize = getRecordSize(schema);
    (*record)->data = (char *)malloc(recordSize);
    if ((*record)->data == NULL)
    {
        free(*record);
        return RC_RM_MEM_ALLOC_FAILED;
    }

    return RC_OK;
}

RC freeRecord(Record *record)
{
    // Free the record data and the Record struct
    if (record != NULL)
    {
        free(record->data);
        free(record);
    }

    return RC_OK;
}

RC getAttr(Record *record, Schema *schema, int attrNum, Value **value)
{
    // Check if the attribute number is valid
    if (attrNum < 0 || attrNum >= schema->numAttr)
    {
        return RC_RM_INVALID_ATTR_NUM;
    }

    // Calculate the offset of the attribute in the record data
    int offset = 0;
    for (int i = 0; i < attrNum; i++)
    {
        switch (schema->dataTypes[i])
        {
        case DT_INT:
            offset += sizeof(int);
            break;
        case DT_FLOAT:
            offset += sizeof(float);
            break;
        case DT_BOOL:
            offset += sizeof(bool);
            break;
        case DT_STRING:
            offset += schema->typeLength[i];
            break;
        }
    }

    // Set the attribute value based on its data type
    *value = (Value *)malloc(sizeof(Value));
    (*value)->dt = schema->dataTypes[attrNum];
    switch (schema->dataTypes[attrNum])
    {
    case DT_INT:
        memcpy(&(*value)->v.intV, record->data + offset, sizeof(int));
        break;
    case DT_FLOAT:
        memcpy(&(*value)->v.floatV, record->data + offset, sizeof(float));
        break;
    case DT_BOOL:
        memcpy(&(*value)->v.boolV, record->data + offset, sizeof(bool));
        break;
    case DT_STRING:
        (*value)->v.stringV = (char *)malloc(schema->typeLength[attrNum] + 1);
        strncpy((*value)->v.stringV, record->data + offset, schema->typeLength[attrNum]);
        (*value)->v.stringV[schema->typeLength[attrNum]] = '\0';
        break;
    }

    return RC_OK;
}

RC setAttr(Record *record, Schema *schema, int attrNum, Value *value)
{
    // Check if the attribute number is valid
    if (attrNum < 0 || attrNum >= schema->numAttr)
    {
        return RC_RM_INVALID_ATTR_NUM;
    }

    // Calculate the offset of the attribute in the record data
    int offset = 0;
    for (int i = 0; i < attrNum; i++)
    {
        switch (schema->dataTypes[i])
        {
        case DT_INT:
            offset += sizeof(int);
            break;
        case DT_FLOAT:
            offset += sizeof(float);
            break;
        case DT_BOOL:
            offset += sizeof(bool);
            break;
        case DT_STRING:
            offset += schema->typeLength[i];
            break;
        }
    }

    // Set the attribute value based on its data type
    switch (schema->dataTypes[attrNum])
    {
    case DT_INT:
        memcpy(record->data + offset, &value->v.intV, sizeof(int));
        break;
    case DT_FLOAT:
        memcpy(record->data + offset, &value->v.floatV, sizeof(float));
        break;
    case DT_BOOL:
        memcpy(record->data + offset, &value->v.boolV, sizeof(bool));
        break;
    case DT_STRING:
        strncpy(record->data + offset, value->v.stringV, schema->typeLength[attrNum]);
        break;
    }

    return RC_OK;
}
