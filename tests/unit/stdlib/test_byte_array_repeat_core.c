/* Canonical growable Array<u8>.repeatFrom owner KAT. */

#include "../test_framework.h"
#include "shared/xr_byte_array_repeat_core.h"

typedef struct TestRepeatStorage {
    uint8_t *data;
    int64_t capacity;
    int calls;
    bool fail;
} TestRepeatStorage;

static bool test_repeat_reserve(void *ctx, XrByteArrayRepeatView *view, int64_t capacity) {
    TestRepeatStorage *storage = (TestRepeatStorage *) ctx;
    storage->calls++;
    if (storage->fail || capacity > storage->capacity)
        return false;
    view->data = storage->data;
    view->capacity = storage->capacity;
    return true;
}

TEST(byte_array_repeat_owner_grows_and_repeats_overlap) {
    uint8_t bytes[9] = {65, 66, 67, 0, 0, 0, 0, 0, 0};
    TestRepeatStorage storage = {bytes, 9, 0, false};
    XrByteArrayRepeatView view = {bytes, 3, 3, XR_ELEM_U8, true};
    XrByteArrayRepeatResult result = XR_BYTE_ARRAY_REPEAT_OWNER_APPLY(
        XR_SEM_OWNER_ID_SHARED_BYTE_ARRAY_REPEAT_HI,
        XR_SEM_OWNER_ID_SHARED_BYTE_ARRAY_REPEAT_LO, XR_SEM_CONSUMER_VM,
        xr_byte_array_repeat_tail_core(&view, 3, 6, test_repeat_reserve, &storage));
    ASSERT_EQ_INT(result.status, XR_BYTE_ARRAY_REPEAT_OK);
    ASSERT(result.changed);
    ASSERT_EQ_INT(result.old_length, 3);
    ASSERT_EQ_INT(result.new_length, 9);
    ASSERT_EQ_INT(view.length, 9);
    ASSERT_EQ_INT(storage.calls, 1);
    for (int i = 0; i < 9; i++)
        ASSERT_EQ_INT(bytes[i], (i % 3) + 65);
}

TEST(byte_array_repeat_owner_rejects_invalid_without_mutation) {
    uint8_t bytes[8] = {1, 2, 3, 4, 0, 0, 0, 0};
    TestRepeatStorage storage = {bytes, 8, 0, false};
    XrByteArrayRepeatView view = {bytes, 4, 8, XR_ELEM_U8, true};
    XrByteArrayRepeatResult result = xr_byte_array_repeat_tail_core(
        &view, 5, 1, test_repeat_reserve, &storage);
    ASSERT_EQ_INT(result.status, XR_BYTE_ARRAY_REPEAT_OUT_OF_BOUNDS);
    ASSERT_EQ_INT(view.length, 4);
    ASSERT_EQ_INT(storage.calls, 0);

    view.elem_type = XR_ELEM_I64;
    result = xr_byte_array_repeat_tail_core(&view, 1, 1, test_repeat_reserve, &storage);
    ASSERT_EQ_INT(result.status, XR_BYTE_ARRAY_REPEAT_WRONG_ELEMENT_TYPE);
    view.elem_type = XR_ELEM_U8;
    view.resizable = false;
    result = xr_byte_array_repeat_tail_core(&view, 1, 1, test_repeat_reserve, &storage);
    ASSERT_EQ_INT(result.status, XR_BYTE_ARRAY_REPEAT_OUT_OF_BOUNDS);
}

TEST(byte_array_repeat_owner_requires_successful_reserve_and_data) {
    uint8_t bytes[4] = {9, 8, 0, 0};
    TestRepeatStorage storage = {bytes, 4, 0, true};
    XrByteArrayRepeatView view = {bytes, 2, 2, XR_ELEM_U8, true};
    XrByteArrayRepeatResult result = xr_byte_array_repeat_tail_core(
        &view, 2, 2, test_repeat_reserve, &storage);
    ASSERT_EQ_INT(result.status, XR_BYTE_ARRAY_REPEAT_RESERVE_FAILED);
    ASSERT_EQ_INT(view.length, 2);
    ASSERT_EQ_INT(bytes[2], 0);

    view.data = NULL;
    view.capacity = 4;
    result = xr_byte_array_repeat_tail_core(&view, 2, 2, test_repeat_reserve, &storage);
    ASSERT_EQ_INT(result.status, XR_BYTE_ARRAY_REPEAT_NO_DATA);
    ASSERT_EQ_INT(view.length, 2);
}

TEST(byte_array_repeat_owner_pins_cross_target_length_limit) {
    XrByteArrayRepeatView view = {(void *) (uintptr_t) 1, INT32_MAX, INT32_MAX,
                                  XR_ELEM_U8, true};
    XrByteArrayRepeatResult result = xr_byte_array_repeat_tail_core(&view, 1, 1, NULL, NULL);
    ASSERT_EQ_INT(result.status, XR_BYTE_ARRAY_REPEAT_OUT_OF_BOUNDS);
    ASSERT_EQ_INT(view.length, INT32_MAX);
}

TEST_MAIN_BEGIN()
RUN_TEST_SUITE("Byte Array Repeat Core");
RUN_TEST(byte_array_repeat_owner_grows_and_repeats_overlap);
RUN_TEST(byte_array_repeat_owner_rejects_invalid_without_mutation);
RUN_TEST(byte_array_repeat_owner_requires_successful_reserve_and_data);
RUN_TEST(byte_array_repeat_owner_pins_cross_target_length_limit);
TEST_MAIN_END()
