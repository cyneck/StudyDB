/**
 * @file test_storage_buffer.c
 * @brief Boundary tests for the storage and buffer managers.
 */
#include <stdio.h>
#include <string.h>

#include "buffer_mgr.h"
#include "storage_mgr.h"
#include "test_helper.h"

char *testName;

static void testStorageBounds(void)
{
    const char *name = "test_storage_bounds.bin";
    char page[PAGE_SIZE];
    SM_FileHandle fh;

    testName = "storage: reject missing pages and grow exactly";
    destroyPageFile(name);
    TEST_CHECK(createPageFile(name));
    TEST_CHECK(openPageFile(name, &fh));
    ASSERT_EQUALS_INT(1, fh.totalNumPages, "new file has one page");
    ASSERT_ERROR(readBlock(1, &fh, page), "reading past EOF fails");
    TEST_CHECK(ensureCapacity(11, &fh));
    ASSERT_EQUALS_INT(11, fh.totalNumPages, "ensureCapacity grows through requested page");
    TEST_CHECK(readBlock(10, &fh, page));
    TEST_CHECK(closePageFile(&fh));
    TEST_CHECK(destroyPageFile(name));
    TEST_DONE();
}

static void testBufferBounds(void)
{
    const char *name = "test_buffer_bounds.bin";
    BM_BufferPool bm;
    BM_PageHandle page;

    testName = "buffer: sparse pin and balanced unpin";
    destroyPageFile(name);
    TEST_CHECK(createPageFile(name));
    TEST_CHECK(initBufferPool(&bm, name, 2, RS_FIFO, NULL));
    TEST_CHECK(pinPage(&bm, &page, 10));
    ASSERT_EQUALS_INT(11, getTotalNumPages(&bm), "pinPage grows file to pageNum + 1");
    memset(page.data, 0x5a, PAGE_SIZE);
    TEST_CHECK(markDirty(&bm, &page));
    TEST_CHECK(unpinPage(&bm, &page));
    ASSERT_ERROR(unpinPage(&bm, &page), "double unpin is rejected");
    TEST_CHECK(shutdownBufferPool(&bm));
    TEST_CHECK(destroyPageFile(name));
    TEST_DONE();
}

int main(void)
{
    initStorageManager();
    testStorageBounds();
    testBufferBounds();
    printf("ALL STORAGE/BUFFER TESTS PASSED\n");
    return 0;
}
