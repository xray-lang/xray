/* Hidden VM grapheme range adapter tests; no public string method is involved. */

#include "../test_framework.h"
#include "../test_helper.h"
#include "runtime/mem/xcoro_heap.h"
#include "runtime/object/xstring.h"
#include "vm/xvm_grapheme_iterator.h"
#include <string.h>

static XrVMRuntime *X = NULL;
static XrCoroutine *main_coro = NULL;

static void setup(void) {
    X = xray_vm_new(NULL);
    ASSERT_NOT_NULL(X);
    main_coro = xr_test_init_coro(X);
    ASSERT_NOT_NULL(main_coro);
}

static void teardown(void) {
    if (X) {
        xray_vm_delete(X);
        X = NULL;
        main_coro = NULL;
    }
}

TEST(vm_grapheme_iterator_keeps_source_alive_without_slice_owner) {
    static const char text[] = "a\xcc\x88"
                               "b";
    XrVmGraphemeIterator iterator = {0};
    XrSliceView span = {0};
    XrByteRange range = {0};
    XrCoroHeap *heap;
    XrString *source;
    uint32_t object_count;

    setup();
    heap = xr_coro_get_heap(main_coro);
    ASSERT_NOT_NULL(heap);
    source = xr_string_new(X, text, sizeof(text) - 1);
    ASSERT_NOT_NULL(source);
    object_count = heap->object_count;
    ASSERT_EQ_INT(source->hdr.refcount, 0);

    ASSERT_TRUE(xr_vm_grapheme_iterator_init(&iterator, source));
    ASSERT_EQ_INT(source->hdr.refcount, 1);

    /* Drop the caller's owner: the internal iterator is now the sole source
     * root, and its next range must remain valid. */
    xr_rc_release(heap, (XrObjHeader *) source);
    ASSERT_EQ_INT(source->hdr.refcount, 0);
    ASSERT_FALSE((source->hdr.extra & XR_OBJ_DEAD) != 0);

    ASSERT_TRUE(xr_vm_grapheme_iterator_next(&iterator, &span, &range));
    ASSERT_EQ_UINT(range.start, 0);
    ASSERT_EQ_UINT(range.end, 3);
    ASSERT_EQ_PTR(span.data, source->data);
    ASSERT_EQ_INT(span.length, 3);
    ASSERT_TRUE(memcmp(span.data, text, 3) == 0);
    ASSERT_EQ_UINT(heap->object_count, object_count);

    ASSERT_TRUE(xr_vm_grapheme_iterator_next(&iterator, &span, &range));
    ASSERT_EQ_UINT(range.start, 3);
    ASSERT_EQ_UINT(range.end, 4);
    ASSERT_EQ_PTR(span.data, source->data + 3);
    ASSERT_EQ_INT(span.length, 1);
    ASSERT_EQ_UINT(heap->object_count, object_count);
    ASSERT_FALSE(xr_vm_grapheme_iterator_next(&iterator, &span, &range));

    xr_vm_grapheme_iterator_dispose(&iterator, heap);
    ASSERT_TRUE((source->hdr.extra & XR_OBJ_DEAD) != 0);
    teardown();
}

TEST(vm_grapheme_iterator_empty_source) {
    XrVmGraphemeIterator iterator = {0};
    XrSliceView span = {0};
    XrCoroHeap *heap;
    XrString *source;

    setup();
    heap = xr_coro_get_heap(main_coro);
    source = xr_string_new(X, "", 0);
    ASSERT_NOT_NULL(source);
    ASSERT_TRUE(xr_vm_grapheme_iterator_init(&iterator, source));
    ASSERT_FALSE(xr_vm_grapheme_iterator_next(&iterator, &span, NULL));
    xr_vm_grapheme_iterator_dispose(&iterator, heap);
    xr_rc_release(heap, (XrObjHeader *) source);
    teardown();
}

TEST_MAIN_BEGIN()

RUN_TEST_SUITE("VM Grapheme - Internal Range Iterator");
RUN_TEST(vm_grapheme_iterator_keeps_source_alive_without_slice_owner);
RUN_TEST(vm_grapheme_iterator_empty_source);

TEST_MAIN_END()
