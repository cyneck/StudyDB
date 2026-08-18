/**
 * @file record_mgr.c
 * @authors Xingli Li
 * @date 2023-07-29
 * @brief Fixed-schema table CRUD and conditional scan implementation.
 */
#include <string.h>
#include <stdlib.h>
#include "storage_mgr.h"
#include "buffer_mgr.h"
#include "record_mgr.h"
#include "record_mgr_ex.h"


/**
 * Setting the global variables and flags
 * @param G_isInitRecordMgr check if the record Manager is inited.
 * @param numOfTuples record the number of tuples in memory.
 * @param pBuffP the buffer pool.
 * @param pPageH the page handler.
 */
bool G_isInitRecordMgr = false;
int numOfTuples = 0;

BM_BufferPool *pBuffP = NULL;
BM_PageHandle *pPageH = NULL;

/**
 * @brief creates a new record manager.Switch the init flag and init storage manager/buffer pool/page handle.
 * @param mgmtData not used in current version.
 * @return RC_OK if init record manager successfully.
 */
RC initRecordManager(void *mgmtData) {
    if (!G_isInitRecordMgr) {
        G_isInitRecordMgr = true;
        initStorageManager();
        pBuffP = MAKE_POOL();
        pPageH = MAKE_PAGE_HANDLE();
        // G_mgmtData = mgmtData;
    }

    return RC_OK;
}

/**
 * @brief destroys a record manager and free up all resources associated with it.
 * @return RC_OK if shutdown successfully.
 */
RC shutdownRecordManager() {
    G_isInitRecordMgr = false;

    free(pBuffP);
    free(pPageH);

    return RC_OK;
}

/**
* @brief save the table schema to the disk.
* @param schema consists of a number of attributes which record the name and data type.
* @return RC_OK if save successfully.
*/
RC saveTableSchema(Schema *schema) {
    /* Four fixed integers precede the per-attribute metadata: schema page
       count, table page count, attribute count, and key count. */
    int sizeSchema = sizeof(int) * (4 + schema->numAttr * 3 + schema->keySize) + sizeof(DataType) * schema->numAttr;
    int attrNameOffset = sizeSchema;
    int offset = 0;

    // size of schema 
    int i;
    for (i = 0; i < schema->numAttr; i++) {
        sizeSchema += strlen(schema->attrNames[i]) + 1;
    }

    // prepare mem buffer
    int numPagesOfSchema = (sizeSchema - 1) / PAGE_SIZE + 1;
    char *buffer = (char *) malloc(PAGE_SIZE * numPagesOfSchema);
    memset(buffer, '\0', PAGE_SIZE * numPagesOfSchema);
    int numPageOfTable = 0;

    // prepare meta data
    MEMCPY_TO_OFFSET(&numPagesOfSchema, int);
    MEMCPY_TO_OFFSET(&numPageOfTable, int);
    MEMCPY_TO_OFFSET(&(schema->numAttr), int);
    MEMCPY_TO_OFFSET(&(schema->keySize), int);

    for (i = 0; i < schema->numAttr; i++) {
        int slen = strlen(schema->attrNames[i]) + 1;
        MEMCPY_TO_OFFSET(&attrNameOffset, int);
        MEMCPY_TO_OFFSET(&slen, int);
        MEMCPY_TO_OFFSET(&(schema->dataTypes[i]), DataType);
        MEMCPY_TO_OFFSET(&(schema->typeLength[i]), int);

        memcpy(&(buffer[attrNameOffset]), schema->attrNames[i], slen);
        attrNameOffset += slen;
    }

    // Key size meta data
    for (i = 0; i < schema->keySize; i++) {
        MEMCPY_TO_OFFSET(&(schema->keyAttrs[i]), int);
    }

    for (i = 0; i < numPagesOfSchema; i++) {
        //CHECKEX(writeBlock(i, fHandle,(SM_PageHandle)(&(buffer[i * PAGE_SIZE]))));  
        pinPage(pBuffP, pPageH, i);
        memcpy(pPageH->data, &(buffer[i * PAGE_SIZE]), PAGE_SIZE);
        markDirty(pBuffP, pPageH);
        unpinPage(pBuffP, pPageH);
    }
    free(buffer);
    return RC_OK;
}

/**
 * @brief create a table by name the schema.
 * @param name the table name.
 * @param schema consists of a number of attributes which record the name and data type.
 * @return RC_OK if create successfully.
 */
RC createTable(char *name, Schema *schema) {
    //  make sure schema valid
    if (!schema) {
        return RC_NULL_POINTER;
    }

    // make sure opened
    if (!G_isInitRecordMgr) {
        return RC_RM_MANAGER_CLOSED;
    }

    numOfTuples = 0;

    //1. create the table file
    CHECKEX(createPageFile(name));

    //2. create the BufferPoll have 10 pages, use FIFO
    initBufferPool(pBuffP, name, 10, RS_FIFO, NULL);

    //3. save table info
    CHECKEX(saveTableSchema(schema));

    //4. close page file
    shutdownBufferPool(pBuffP);

    return RC_OK;
}

/**
 * @brief read a table schema.
 * @return schema consists of a number of attributes which record the name and data type.
 */
Schema *readTableSchema() {
    Schema *schema = (Schema *) malloc(sizeof(Schema));
    int numPagesOfSchema = 0;
    pinPage(pBuffP, pPageH, 0);
    memcpy(&numPagesOfSchema, pPageH->data, sizeof(int));
    unpinPage(pBuffP, pPageH);

    int i;
    char *buffer = (char *) malloc(PAGE_SIZE * numPagesOfSchema);

    //  read data to buffer from each page
    for (i = 0; i < numPagesOfSchema; i++) {
        pinPage(pBuffP, pPageH, i);
        memcpy(&buffer[i * PAGE_SIZE], pPageH->data, PAGE_SIZE);
        unpinPage(pBuffP, pPageH);
    }

    int numPageOfTable = 0;

    //  get data from buffer
    int offset = sizeof(int);
    MEMCPY_FROM_OFFSET(&numPageOfTable, int);
    MEMCPY_FROM_OFFSET(&schema->numAttr, int);
    MEMCPY_FROM_OFFSET(&schema->keySize, int);

    schema->attrNames = (char **) malloc(sizeof(char *) * schema->numAttr);
    schema->dataTypes = (DataType *) malloc(sizeof(DataType) * schema->numAttr);
    schema->typeLength = (int *) malloc(sizeof(int *) * schema->numAttr);
    schema->keyAttrs = (int *) malloc(sizeof(int *) * schema->keySize);

    int attrNameOffset, slen;
    for (i = 0; i < schema->numAttr; i++) {
        MEMCPY_FROM_OFFSET(&attrNameOffset, int);
        MEMCPY_FROM_OFFSET(&slen, int);
        MEMCPY_FROM_OFFSET(&schema->dataTypes[i], DataType);
        MEMCPY_FROM_OFFSET(&schema->typeLength[i], int);

        schema->attrNames[i] = (char *) malloc(slen);
        memcpy(schema->attrNames[i], &buffer[attrNameOffset], slen);
    }

    for (i = 0; i < schema->keySize; i++) {
        MEMCPY_FROM_OFFSET(&schema->keyAttrs[i], int);
    }
    free(buffer);
    return schema;
}

/**
 * @brief open the specified table.
 * @param rel a record manager to handle one relation.
 * @param name the table name.
 * @return RC_OK if open successfully.
 **/
RC openTable(RM_TableData *rel, char *name) {
    //  verify input
    if (!rel || !name) {
        return RC_NULL_POINTER;
    }

    initBufferPool(pBuffP, name, 10, RS_FIFO, NULL);
    if (!pBuffP->isOpen) {
        return RC_FILE_NOT_FOUND;
    }
    Schema *schema = readTableSchema();

    /* Rebuild the live tuple count from persisted data pages. The previous
       implementation copied a process-global counter, which became stale
       after reopening a table or switching between tables. */
    int numDataPages = 0;
    pinPage(pBuffP, pPageH, 0);
    memcpy(&numDataPages, pPageH->data + sizeof(int), sizeof(int));
    unpinPage(pBuffP, pPageH);

    int recordSize = getRecordSize(schema);
    int liveTuples = 0;
    for (int page = 1; page <= numDataPages && recordSize > 0; page++) {
        int recordsOnPage = 0;
        pinPage(pBuffP, pPageH, page);
        memcpy(&recordsOnPage, pPageH->data, sizeof(int));
        if (recordsOnPage == -1)
            recordsOnPage = (PAGE_SIZE - (int) sizeof(int)) / recordSize;
        for (int slot = 0; slot < recordsOnPage; slot++) {
            char *data = pPageH->data + sizeof(int) + slot * recordSize;
            bool deleted = recordSize >= 3 &&
                           data[0] == '-' && data[1] == 'D' && data[2] == '-';
            if (!deleted)
                liveTuples++;
        }
        unpinPage(pBuffP, pPageH);
    }

    // init the table info and metadata
    TableInfo *table = (TableInfo *) malloc(sizeof(TableInfo));
    table->numOfTuples = liveTuples;
    numOfTuples = liveTuples;

    rel->name = name;
    rel->schema = schema;
    rel->mgmtData = table;

    return RC_OK;
}

/**
 * @brief open a specified table.
 * @param rel a record manager to handle one relation.
 * @param name the table name.
 * @return RC_OK if open successfully.
 **/
RC closeTable(RM_TableData *rel) {
    if (!rel) {
        return RC_NULL_POINTER;
    }

    if (rel->mgmtData) {
        free(rel->mgmtData);
    }

    freeSchema(rel->schema);
    shutdownBufferPool(pBuffP);

    return RC_OK;
}

/**
 * @brief delete a specified table.
 * @param name the table name.
 * @return RC_OK if open successfully, return RC_NULL_POINTER if name if NULL.
 **/
RC deleteTable(char *name) {
    if (!name) {
        return RC_NULL_POINTER;
    }
    numOfTuples = 0;
    destroyPageFile(name);
    return RC_OK;
}

/**
 * @brief get the number of the tuple in the specified table.
 * @param rel a record manager to handle one relation.
 * @return the number of the tuple in the table.
 **/
int getNumTuples(RM_TableData *rel) {
    return ((TableInfo *) rel->mgmtData)->numOfTuples;
}

/**
 * @brief update the page number of the table.
 * @param num the page number.
**/
void updateNumPageOfTable(int num) {
    pinPage(pBuffP, pPageH, 0);
    memcpy(pPageH->data + sizeof(int), &num, sizeof(int));
    markDirty(pBuffP, pPageH);
    unpinPage(pBuffP, pPageH);
    forceFlushPool(pBuffP);
}

/**
 * @brief insert a record into a specified table.
 * @param rel a record manager to handle one relation.
 * @param record the record need to insert into the table.
 * @return RC_OK if insert record successfully.
**/
RC insertRecord(RM_TableData *rel, Record *record) {
    if (!rel || !record) {
        return RC_NULL_POINTER;
    }
    int numPagesOfTable = 0;

    pinPage(pBuffP, pPageH, 0);
    memcpy(&numPagesOfTable, pPageH->data + sizeof(int), sizeof(int));
    unpinPage(pBuffP, pPageH);

    Schema *schema = rel->schema;
    PageSlot pos;

    int numRecordInPage = 0;

    if (0 == numPagesOfTable) {
        numPagesOfTable++;
        updateNumPageOfTable(numPagesOfTable);

        numRecordInPage = 0;
        pinPage(pBuffP, pPageH, 1);
        memcpy(pPageH->data, &numRecordInPage, sizeof(int));
        markDirty(pBuffP, pPageH);
        unpinPage(pBuffP, pPageH);
        forceFlushPool(pBuffP);

        pos.page_id = numPagesOfTable;
        pos.slot_id = 0;
    } else {
        int tmpNum = 0;
        pinPage(pBuffP, pPageH, numPagesOfTable);
        memcpy(&tmpNum, pPageH->data, sizeof(int));
        unpinPage(pBuffP, pPageH);
        pos.page_id = numPagesOfTable;
        pos.slot_id = tmpNum;

        if (tmpNum == -1) {
            numPagesOfTable++;
            updateNumPageOfTable(numPagesOfTable);

            numRecordInPage = 0;
            pinPage(pBuffP, pPageH, numPagesOfTable);
            memcpy(pPageH->data, &numRecordInPage, sizeof(int));
            markDirty(pBuffP, pPageH);
            unpinPage(pBuffP, pPageH);
            forceFlushPool(pBuffP);

            pos.page_id = numPagesOfTable;
            pos.slot_id = 0;
        }
    }

    //  set record page and slot
    record->id.page = pos.page_id;
    record->id.slot = pos.slot_id;

    //  get record data based on pos
    int recordSize = getRecordSize(schema);
    int offset = pos.slot_id * recordSize + sizeof(int);
    pinPage(pBuffP, pPageH, numPagesOfTable);
    memcpy((char *) pPageH->data + offset, record->data, recordSize);
    numRecordInPage = pos.slot_id + 1;
    // check if the page was full
    if ((numRecordInPage + 1) * recordSize + sizeof(int) > PAGE_SIZE)
        numRecordInPage = -1;
    memcpy(pPageH->data, &numRecordInPage, sizeof(int));
    markDirty(pBuffP, pPageH);
    unpinPage(pBuffP, pPageH);
    forceFlushPool(pBuffP);

    numOfTuples++;
    ((TableInfo *) rel->mgmtData)->numOfTuples++;
    return RC_OK;
}

/**
 * @brief delete a record of a specified table.
 * @param rel a record manager to handle one relation.
 * @param id the record id need to delete of the table.
 * @return RC_OK if delete record successfully.
**/
RC deleteRecord(RM_TableData *rel, RID id) {
    if (!rel) {
        return RC_NULL_POINTER;
    }

    int page_id = id.page;
    int slot_id = id.slot;
    Schema *schema = rel->schema;
    int recordSize = getRecordSize(schema);

    char *data = (char *) malloc(sizeof(char) * recordSize);
    memset(data, '\0', sizeof(char) * recordSize);
    data[0] = '-';
    data[1] = 'D';
    data[2] = '-';

    int offset = slot_id * recordSize + sizeof(int);
    pinPage(pBuffP, pPageH, page_id);
    memcpy((char *) pPageH->data + offset, data, recordSize);
    markDirty(pBuffP, pPageH);
    unpinPage(pBuffP, pPageH);
    forceFlushPool(pBuffP);

    free(data);

    numOfTuples--;
    ((TableInfo *) rel->mgmtData)->numOfTuples--;
    return RC_OK;
}

/**
 * @brief update a record of a specified table.
 * @param rel a record manager to handle one relation.
 * @param record the record need to update of the table.
 * @return RC_OK if update record successfully.
**/
RC updateRecord(RM_TableData *rel, Record *record) {
    if (!rel) {
        return RC_NULL_POINTER;
    }

    int page_id = record->id.page;
    int slot_id = record->id.slot;
    Schema *schema = rel->schema;
    int recordSize = getRecordSize(schema);

    int offset = slot_id * recordSize + sizeof(int);
    pinPage(pBuffP, pPageH, page_id);
    memcpy((char *) pPageH->data + offset, record->data, recordSize);
    markDirty(pBuffP, pPageH);
    unpinPage(pBuffP, pPageH);
    forceFlushPool(pBuffP);

    return RC_OK;
}

/**
 * @brief get a record from a specified table by id.
 * @param rel a record manager to handle one relation.
 * @param id the record id of the record want to get.
 * @param record the pointer of the record.
 * @return RC_OK if find the record successfully.
**/
RC getRecord(RM_TableData *rel, RID id, Record *record) {
    if (!rel || !record) {
        return RC_NULL_POINTER;
    }

    int page_id = id.page;
    int slot_id = id.slot;
    int recordSize = getRecordSize(rel->schema);

    int offset = slot_id * recordSize + sizeof(int);
    pinPage(pBuffP, pPageH, page_id);
    memcpy(record->data, (char *) pPageH->data + offset, recordSize);
    unpinPage(pBuffP, pPageH);

    if ('-' == record->data[0] && 'D' == record->data[1] && '-' == record->data[2])
        return RC_RM_NO_MORE_TUPLES;


    return RC_OK;
}

/**
 * @brief Starting a scan initializes the RM ScanHandle data structure.
 * @param rel a record manager to handle one relation.
 * @param scan a data structure handle the scan feature.
 * @param cond the condition from SQL.
 * @return RC_OK if start scan successfully.
**/
RC startScan(RM_TableData *rel, RM_ScanHandle *scan, Expr *cond) {
    //  validate (cond may be NULL, meaning "scan all records")
    if (!rel || !scan)
        return RC_NULL_POINTER;

    // get num of pages
    int numPages = 0;
    pinPage(pBuffP, pPageH, 0);
    memcpy(&numPages, pPageH->data + sizeof(int), sizeof(int));
    unpinPage(pBuffP, pPageH);

    Scanner *sc = (Scanner *) malloc(sizeof(Scanner));
    sc->page = 1;
    sc->slot = 0;
    sc->lastPage = numPages;
    sc->cond = cond;

    scan->rel = rel;
    scan->mgmtData = sc;

    return RC_OK;
}

/**
 * @brief get the next result from the scan handler.
 * @param scan a data structure handle the scan feature.
 * @param record the pointer of the next record.
 * @return RC_OK if scan next successfully.
**/
RC next(RM_ScanHandle *scan, Record *record) {
    if (!scan || !scan->rel || !scan->mgmtData || !record) {
        return RC_NULL_POINTER;
    }
    RM_TableData *rel = scan->rel;
    Scanner *sc = (Scanner *) scan->mgmtData;
    int recordSize = getRecordSize(rel->schema);
    if (recordSize <= 0)
        return RC_RM_NO_MORE_TUPLES;

    while (sc->page <= sc->lastPage) {
        int recordsOnPage;
        CHECKEX(pinPage(pBuffP, pPageH, sc->page));
        memcpy(&recordsOnPage, pPageH->data, sizeof(int));
        CHECKEX(unpinPage(pBuffP, pPageH));

        if (recordsOnPage == -1)
            recordsOnPage = (PAGE_SIZE - (int) sizeof(int)) / recordSize;

        if (sc->slot >= recordsOnPage) {
            sc->page++;
            sc->slot = 0;
            continue;
        }

        RID rid = {sc->page, sc->slot++};
        RC rc = getRecord(rel, rid, record);
        if (rc == RC_RM_NO_MORE_TUPLES)
            continue; /* deleted slot */
        if (rc != RC_OK)
            return rc;

        record->id = rid;
        if (!sc->cond)
            return RC_OK;

        Value *value = NULL;
        rc = evalExpr(record, rel->schema, sc->cond, &value);
        if (rc != RC_OK)
            return rc;
        bool matches = value->v.boolV;
        freeVal(value);
        if (matches)
            return RC_OK;
    }

    return RC_RM_NO_MORE_TUPLES;
}

/**
 * @brief close the scan handler and free related resources.
 * @param scan a data structure handle the scan feature.
 * @return RC_OK if close scan handler successfully.
**/
RC closeScan(RM_ScanHandle *scan) {
    if(scan->mgmtData) {
        free(scan->mgmtData);
    }
//    free(scan);
    return RC_OK;
}

/**
 * @brief get the record size according to the schema
 * @param schema consists of a number of attributes which record the name and data type.
 * @return the size of the record.
**/
int getRecordSize(Schema *schema) {
    if (!schema) {
        return 0;
    }

    // loop all attribute size and sum
    int i, size = 0;
    for (i = 0; i < schema->numAttr; i++) {
        switch (schema->dataTypes[i]) {
            case DT_INT:
                size += sizeof(int);
                break;
            case DT_STRING:
                size += schema->typeLength[i];
                break;
            case DT_FLOAT:
                size += sizeof(float);
                break;
            case DT_BOOL:
                size += sizeof(bool);
                break;
            default:
                break;
        }
    }

    return size;
}

/**
 * @brief create the schema with provided parameters.
 * @param numAttr the attribute number of the schema.
 * @param attrNames names of each attributes.
 * @param dataTypes data type of each attributes.
 * @param typeLength the size of the strings for attributes of type DT_STRING.
 * @param keySize the key attribute size.
 * @param keys the key attribute of the schema.
 * @return the schema created with provided parameters.
**/
Schema *createSchema(int numAttr, char **attrNames, const DataType *dataTypes,
                     const int *typeLength, int keySize, const int *keys) {
    if (numAttr <= 0) {
        return NULL;
    }

    Schema *schema = (Schema *) malloc(sizeof(Schema));

    schema->numAttr = numAttr;
    schema->keySize = keySize;

    schema->attrNames = (char **) malloc(sizeof(char *) * numAttr);
    schema->dataTypes = (DataType *) malloc(sizeof(DataType) * numAttr);
    schema->typeLength = (int *) malloc(sizeof(int) * numAttr);
    schema->keyAttrs = (int *) malloc(sizeof(int) * keySize);

    int i;
    for (i = 0; i < numAttr; i++) {
        schema->attrNames[i] = attrNames[i];
        schema->dataTypes[i] = dataTypes[i];
        schema->typeLength[i] = typeLength[i];
    }

    for (i = 0; i < keySize; i++) {
        schema->keyAttrs[i] = keys[i];
    }

    return schema;
}

/**
 * @brief release the schema resources.
 * @param schema the schema need to release.
 * @return RC_OK if free schema successfully.
**/
RC freeSchema(Schema *schema) {
    if (!schema) {
        return RC_NULL_POINTER;
    }

    int i = 0;
    for( i = 0; i < schema->numAttr;i++) {
        free(schema->attrNames[i]);
    }

    free(schema->attrNames);
    free(schema->dataTypes);
    free(schema->typeLength);
    free(schema->keyAttrs);

    free(schema);

    return RC_OK;
}

/**
 * @brief release the schema resources.
 * @param record the record is going to create.
 * @param schema the table schema.
 * @return RC_OK if create record successfully.
**/
RC createRecord(Record **record, Schema *schema) {
    if (!record || !schema) {
        return RC_NULL_POINTER;
    }

    Record *r = (Record *) malloc(sizeof(Record));
    char *data = (char *) malloc(sizeof(char) * getRecordSize(schema));
    memset(data, '\0', sizeof(char) * getRecordSize(schema));

    r->id = (RID){.page=0, .slot=0};;
    r->data = data;
    *(record) = r;

    return RC_OK;
}

/**
 * @brief release the schema resources.
 * @param record the record is going to create.
 * @param schema the table schema.
 * @return RC_OK if create record successfully.
**/
RC freeRecord(Record *record) {
    if (!record) {
        return RC_NULL_POINTER;
    }

    free(record->data);
    free(record);

    return RC_OK;
}

/**
 * @brief get attribute from a record with specified schema by attribute number.
 * @param record the record is going to get attribute value.
 * @param schema the table schema.
 * @param attrNum the target attribute key number.
 * @param value value of the target attribute .
 * @return RC_OK if get attribute successfully.
**/
RC getAttr(Record *record, Schema *schema, int attrNum, Value **value) {
    if (!record) {
        return RC_NULL_POINTER;
    }

    Value *v = (Value *) malloc(sizeof(Value));
    int i = 0;
    unsigned long offset = 0;
    // calculate the offset
    while (i < attrNum) {
        offset += schema->dataTypes[i] == DT_STRING ? (schema->typeLength[i]) * sizeof(char): sizeof(schema->dataTypes[i]);
        i++;
    }

    // get value
    switch (schema->dataTypes[attrNum]) {
        case DT_INT:
            memcpy(&(v->v.intV), &(record->data[offset]), sizeof(int));
            v->dt = DT_INT;
            break;
        case DT_STRING:
            v->v.stringV = malloc((schema->typeLength[i]) * sizeof(char) + 1);
            memset(v->v.stringV, '\0', (schema->typeLength[i]) * sizeof(char) + 1);
            memcpy(v->v.stringV, &(record->data[offset]), (schema->typeLength[i]) * sizeof(char));
            v->dt = DT_STRING;
            break;
        case DT_FLOAT:
            memcpy(&(v->v.floatV), &(record->data[offset]), sizeof(float));
            v->dt = DT_FLOAT;
            break;
        case DT_BOOL:
            memcpy(&(v->v.boolV), &(record->data[offset]), sizeof(bool));
            v->dt = DT_BOOL;
            break;
    }

    (*value) = v;

    return RC_OK;
}

/**
 * @brief set attribute from a record with specified schema by attribute number.
 * @param record the record is going to set attribute value.
 * @param schema the table schema.
 * @param attrNum the target attribute key number.
 * @param value value of the target attribute going to set.
 * @return RC_OK if set attribute successfully.
**/
RC setAttr(Record *record, Schema *schema, int attrNum, Value *value) {
    if (!record || !schema || !value) {
        return RC_NULL_POINTER;
    }
    if (attrNum < 0 || attrNum >= schema->numAttr ||
        value->dt != schema->dataTypes[attrNum])
        return RC_RM_UNKOWN_DATATYPE;

    // calculate the offset
    int i = 0, offset = 0;
    while (i < attrNum) {
        offset += schema->dataTypes[i] == DT_STRING ? (schema->typeLength[i]) * sizeof(char): sizeof(schema->dataTypes[i]);
        i++;
    }

    // set value
    switch (value->dt) {
        case DT_INT:
            memcpy(&(record->data[offset]), &value->v.intV, sizeof(int));
            break;
        case DT_STRING:
        {
            size_t fieldLength = (size_t) schema->typeLength[attrNum];
            size_t valueLength = strlen(value->v.stringV);
            memset(&record->data[offset], '\0', fieldLength);
            memcpy(&record->data[offset], value->v.stringV,
                   valueLength < fieldLength ? valueLength : fieldLength);
            break;
        }
        case DT_FLOAT:
            memcpy(&(record->data[offset]), &value->v.floatV, sizeof(float));
            break;
        case DT_BOOL:
            memcpy(&(record->data[offset]), &value->v.boolV, sizeof(bool));
            break;
    }

    return RC_OK;
}
