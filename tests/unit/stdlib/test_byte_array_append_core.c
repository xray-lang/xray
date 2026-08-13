/* Canonical growable Array<byte>.appendFrom owner KAT. */

#include "../test_framework.h"
#include "shared/xr_byte_array_append_core.h"
#include <string.h>

typedef struct TestAppendStorage {
    uint8_t *data;
    int64_t capacity;
    int calls;
    bool fail;
} TestAppendStorage;

static bool test_append_reserve(void *ctx, XrByteArrayAppendView *view, int64_t capacity) {
    TestAppendStorage *storage = (TestAppendStorage *) ctx;
    storage->calls++;
    if (storage->fail || capacity > storage->capacity)
        return false;
    if (view->length > 0 && view->data != storage->data)
        memcpy(storage->data, view->data, (size_t) view->length);
    view->data = storage->data;
    view->capacity = storage->capacity;
    return true;
}

TEST(byte_array_append_owner_appends_external_span) {
    uint8_t bytes[6] = {1, 2, 0, 0, 0, 0};
    uint8_t source[3] = {3, 4, 5};
    XrByteArrayAppendView view = {bytes, 2, 6, XR_ELEM_U8, true, bytes};
    XrByteArrayAppendResult result = XR_BYTE_ARRAY_APPEND_OWNER_APPLY(
        XR_SEM_OWNER_ID_SHARED_BYTE_ARRAY_APPEND_HI,
        XR_SEM_OWNER_ID_SHARED_BYTE_ARRAY_APPEND_LO, XR_SEM_CONSUMER_VM,
        xr_byte_array_append_core(&view, source, 3, XR_ELEM_U8, source, NULL, NULL));
    ASSERT_EQ_INT(result.status, XR_BYTE_ARRAY_APPEND_OK);
    ASSERT(result.changed);
    ASSERT_EQ_INT(result.old_length, 2);
    ASSERT_EQ_INT(result.new_length, 5);
    ASSERT_EQ_INT(view.length, 5);
    ASSERT_EQ_INT(bytes[2], 3);
    ASSERT_EQ_INT(bytes[4], 5);
}

TEST(byte_array_append_owner_preserves_alias_across_growth) {
    uint8_t old_bytes[3] = {7, 8, 9};
    uint8_t grown[6] = {0};
    int identity = 0;
    TestAppendStorage storage = {grown, 6, 0, false};
    XrByteArrayAppendView view = {old_bytes, 3, 3, XR_ELEM_U8, true, &identity};
    XrByteArrayAppendResult result = xr_byte_array_append_core(
        &view, old_bytes, 3, XR_ELEM_U8, &identity, test_append_reserve, &storage);
    ASSERT_EQ_INT(result.status, XR_BYTE_ARRAY_APPEND_OK);
    ASSERT_EQ_INT(storage.calls, 1);
    ASSERT_EQ_INT(view.length, 6);
    for (int i = 0; i < 6; i++)
        ASSERT_EQ_INT(grown[i], (i % 3) + 7);
}

TEST(byte_array_append_owner_rejects_invalid_without_mutation) {
    uint8_t bytes[4] = {1, 2, 0, 0};
    uint8_t source[2] = {3, 4};
    int identity = 0;
    XrByteArrayAppendView view = {bytes, 2, 4, XR_ELEM_U8, true, &identity};
    XrByteArrayAppendResult result = xr_byte_array_append_core(
        &view, source, 2, XR_ELEM_I64, source, NULL, NULL);
    ASSERT_EQ_INT(result.status, XR_BYTE_ARRAY_APPEND_WRONG_ELEMENT_TYPE);
    ASSERT_EQ_INT(view.length, 2);

    result = xr_byte_array_append_core(&view, source, 2, XR_ELEM_U8, &identity, NULL, NULL);
    ASSERT_EQ_INT(result.status, XR_BYTE_ARRAY_APPEND_OUT_OF_BOUNDS);
    ASSERT_EQ_INT(view.length, 2);
    ASSERT_EQ_INT(bytes[2], 0);
}

TEST(byte_array_append_owner_pins_growth_and_cross_target_limit) {
    uint8_t old_bytes[2] = {1, 2};
    uint8_t grown[4] = {0};
    TestAppendStorage storage = {grown, 4, 0, true};
    XrByteArrayAppendView view = {old_bytes, 2, 2, XR_ELEM_U8, true, old_bytes};
    XrByteArrayAppendResult result = xr_byte_array_append_core(
        &view, old_bytes, 2, XR_ELEM_U8, old_bytes, test_append_reserve, &storage);
    ASSERT_EQ_INT(result.status, XR_BYTE_ARRAY_APPEND_RESERVE_FAILED);
    ASSERT_EQ_INT(view.length, 2);

    view.data = (void *) (uintptr_t) 1;
    view.length = INT32_MAX;
    view.capacity = INT32_MAX;
    result = xr_byte_array_append_core(&view, old_bytes, 1, XR_ELEM_U8, old_bytes, NULL, NULL);
    ASSERT_EQ_INT(result.status, XR_BYTE_ARRAY_APPEND_OUT_OF_BOUNDS);
    ASSERT_EQ_INT(view.length, INT32_MAX);
}

TEST_MAIN_BEGIN()
RUN_TEST_SUITE("Byte Array Append Core");
RUN_TEST(byte_array_append_owner_appends_external_span);
RUN_TEST(byte_array_append_owner_preserves_alias_across_growth);
RUN_TEST(byte_array_append_owner_rejects_invalid_without_mutation);
RUN_TEST(byte_array_append_owner_pins_growth_and_cross_target_limit);
TEST_MAIN_END()
