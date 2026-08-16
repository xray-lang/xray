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
    XrVMConfig params = {0};
    X = xray_vm_new_full(&params);
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

static void release_string(XrString *string) {
    bool last = false;
    ASSERT_EQ_INT(xr_runtime_object_header_release(&string->header, &last),
                  XR_RUNTIME_ABI_OK);
    if (last)
        ASSERT_EQ_INT(xr_runtime_object_reclaim(&string->header),
                      XR_RUNTIME_ABI_OK);
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
    ASSERT_EQ_INT(atomic_load_explicit(&source->header.rc, memory_order_relaxed), 1);

    ASSERT_TRUE(xr_grapheme_iterator_init(&iterator, source));
    ASSERT_EQ_INT(atomic_load_explicit(&source->header.rc, memory_order_relaxed), 2);

    /* Drop the caller's owner: the internal iterator is now the sole source
     * root, and its next range must remain valid. */
    release_string(source);
    ASSERT_EQ_INT(atomic_load_explicit(&source->header.rc, memory_order_relaxed), 1);

    ASSERT_TRUE(xr_grapheme_iterator_next(&iterator, &span, &range));
    ASSERT_EQ_UINT(range.start, 0);
    ASSERT_EQ_UINT(range.end, 3);
    ASSERT_EQ_PTR(span.data, source->data);
    ASSERT_EQ_INT(span.length, 3);
    ASSERT_TRUE(memcmp(span.data, text, 3) == 0);
    ASSERT_EQ_UINT(heap->object_count, object_count);

    ASSERT_TRUE(xr_grapheme_iterator_next(&iterator, &span, &range));
    ASSERT_EQ_UINT(range.start, 3);
    ASSERT_EQ_UINT(range.end, 4);
    ASSERT_EQ_PTR(span.data, source->data + 3);
    ASSERT_EQ_INT(span.length, 1);
    ASSERT_EQ_UINT(heap->object_count, object_count);
    ASSERT_FALSE(xr_grapheme_iterator_next(&iterator, &span, &range));

    xr_grapheme_iterator_dispose(&iterator);
    ASSERT_EQ_UINT(heap->object_count, object_count - 1);
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
    ASSERT_TRUE(xr_grapheme_iterator_init(&iterator, source));
    ASSERT_FALSE(xr_grapheme_iterator_next(&iterator, &span, NULL));
    xr_grapheme_iterator_dispose(&iterator);
    release_string(source);
    teardown();
}

TEST_MAIN_BEGIN()

RUN_TEST_SUITE("VM Grapheme - Internal Range Iterator");
RUN_TEST(vm_grapheme_iterator_keeps_source_alive_without_slice_owner);
RUN_TEST(vm_grapheme_iterator_empty_source);

TEST_MAIN_END()
