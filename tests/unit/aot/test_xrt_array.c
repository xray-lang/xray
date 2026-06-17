#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define XRT_DATA_ALIGN 32

static int g_passed;
static int g_failed;
static int g_malloc_count;
static int g_aligned_alloc_count;
static int g_aligned_free_count;

static void *test_malloc(size_t size) {
    g_malloc_count++;
    return malloc(size);
}

static void *test_calloc(size_t count, size_t size) {
    g_malloc_count++;
    return calloc(count, size);
}

static void *test_realloc(void *ptr, size_t size) {
    if (!ptr)
        g_malloc_count++;
    return realloc(ptr, size);
}

static void test_free(void *ptr) {
    free(ptr);
}

static void *test_alloc_aligned(size_t size) {
    void *ptr = NULL;
    g_aligned_alloc_count++;
    if (posix_memalign(&ptr, XRT_DATA_ALIGN, size) != 0)
        return NULL;
    return ptr;
}

static void test_free_aligned(void *ptr) {
    if (ptr)
        g_aligned_free_count++;
    free(ptr);
}

#define XRT_MALLOC(sz) test_malloc(sz)
#define XRT_CALLOC(n, sz) test_calloc((n), (sz))
#define XRT_REALLOC(p, sz) test_realloc((p), (sz))
#define XRT_FREE(p) test_free(p)
#define XRT_ALLOC_ALIGNED(sz) test_alloc_aligned(sz)
#define XRT_FREE_ALIGNED(p) test_free_aligned(p)

#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wundefined-internal"
#pragma clang diagnostic ignored "-Wunused-function"
#endif
#include "../../../src/aot/xrt_coll.h"
#if defined(__clang__)
#pragma clang diagnostic pop
#endif

#define ASSERT_TRUE(cond, msg)                                                                     \
    do {                                                                                           \
        if (!(cond)) {                                                                             \
            fprintf(stderr, "FAIL: %s\n", msg);                                                    \
            g_failed++;                                                                            \
            return;                                                                                \
        }                                                                                          \
        g_passed++;                                                                                \
    } while (0)

#define ASSERT_EQ_INT(actual, expected, msg)                                                       \
    do {                                                                                           \
        if ((int64_t) (actual) != (int64_t) (expected)) {                                          \
            fprintf(stderr, "FAIL: %s (got %lld, expected %lld)\n", msg, (long long) (actual),     \
                    (long long) (expected));                                                       \
            g_failed++;                                                                            \
            return;                                                                                \
        }                                                                                          \
        g_passed++;                                                                                \
    } while (0)

static void reset_alloc_counts(void) {
    g_malloc_count = 0;
    g_aligned_alloc_count = 0;
    g_aligned_free_count = 0;
}

static int ptr_is_aligned(const void *ptr) {
    return ((uintptr_t) ptr & (uintptr_t) (XRT_DATA_ALIGN - 1)) == 0;
}

static void free_test_array(xrt_array_t *a) {
    if (!a)
        return;
    if (xrt_array_data_is_heap(a))
        XRT_FREE_ALIGNED(a->data);
    XRT_FREE(a);
}

static void test_small_array_uses_inline_storage(void) {
    reset_alloc_counts();
    XrValue value = xrt_array_new(2);
    xrt_array_t *a = (xrt_array_t *) value.ptr;

    ASSERT_TRUE(a != NULL, "array allocated");
    ASSERT_EQ_INT(a->cap, 4, "plain array minimum cap is 4");
    ASSERT_TRUE(xrt_array_data_is_inline(a), "plain array starts with inline data");
    ASSERT_TRUE(ptr_is_aligned(a->data), "inline plain array data is XRT_DATA_ALIGN-aligned");
    ASSERT_EQ_INT(g_malloc_count, 1, "plain array uses one malloc for header plus data");
    ASSERT_EQ_INT(g_aligned_alloc_count, 0, "plain array constructor does not heap-allocate data");
    ASSERT_EQ_INT(((XrValue *) a->data)[0].tag, XR_TAG_NULL, "inline plain data is zeroed");

    free_test_array(a);
}

static void test_typed_exact_zero_uses_header_only(void) {
    reset_alloc_counts();
    XrValue value = xrt_array_new_typed_exact(0, XR_ELEM_U8);
    xrt_array_t *a = (xrt_array_t *) value.ptr;

    ASSERT_TRUE(a != NULL, "exact zero typed array allocated");
    ASSERT_EQ_INT(a->cap, 0, "exact zero typed array keeps cap 0");
    ASSERT_TRUE(xrt_array_data_is_inline(a), "exact zero typed array is inline storage domain");
    ASSERT_TRUE(a->data == NULL, "exact zero typed array has no data pointer");
    ASSERT_EQ_INT(g_malloc_count, 1, "exact zero typed array uses one header allocation");
    ASSERT_EQ_INT(g_aligned_alloc_count, 0, "exact zero typed array does not allocate data");

    free_test_array(a);
}

static void test_growth_spills_inline_to_heap_and_preserves_values(void) {
    reset_alloc_counts();
    XrValue value = xrt_array_new(4);
    xrt_array_t *a = (xrt_array_t *) value.ptr;

    for (int64_t i = 0; i < 5; i++)
        xrt_array_push(value, XR_FROM_INT(i + 10));

    ASSERT_TRUE(xrt_array_data_is_heap(a), "first growth spills inline data to heap");
    ASSERT_EQ_INT(a->cap, 8, "first growth doubles cap");
    ASSERT_EQ_INT(g_aligned_alloc_count, 1, "first growth allocates one heap data buffer");
    ASSERT_EQ_INT(g_aligned_free_count, 0, "first growth does not free inline data");
    ASSERT_TRUE(ptr_is_aligned(a->data), "grown data is XRT_DATA_ALIGN-aligned");
    ASSERT_EQ_INT(((XrValue *) a->data)[0].i, 10, "first element survives inline spill");
    ASSERT_EQ_INT(((XrValue *) a->data)[4].i, 14, "new element stored after spill");

    for (int64_t i = 5; i < 9; i++)
        xrt_array_push(value, XR_FROM_INT(i + 10));

    ASSERT_TRUE(xrt_array_data_is_heap(a), "second growth remains heap storage");
    ASSERT_EQ_INT(a->cap, 16, "second growth doubles cap");
    ASSERT_EQ_INT(g_aligned_alloc_count, 2, "second growth allocates replacement heap buffer");
    ASSERT_EQ_INT(g_aligned_free_count, 1, "second growth frees previous heap buffer");
    ASSERT_EQ_INT(((XrValue *) a->data)[8].i, 18, "value survives second growth");

    free_test_array(a);
}

static void test_slice_marks_borrowed_storage(void) {
    reset_alloc_counts();
    XrValue value = xrt_array_new(4);
    xrt_array_t *a = (xrt_array_t *) value.ptr;
    for (int64_t i = 0; i < 4; i++)
        xrt_array_push(value, XR_FROM_INT(i));

    XrValue slice_value = xrt_array_slice_view(value, 1, 3);
    xrt_array_t *slice = (xrt_array_t *) slice_value.ptr;
    ASSERT_TRUE(slice != NULL, "slice allocated");
    ASSERT_EQ_INT(slice->data_storage, XRT_ARRAY_DATA_BORROWED, "slice marked BORROWED");
    ASSERT_TRUE(xrt_array_data_is_borrowed(slice), "slice data is borrowed");
    ASSERT_EQ_INT(((XrValue *) slice->data)[0].i, 1, "slice points into source data");

    free_test_array(slice);
    free_test_array(a);
}

int main(void) {
    test_small_array_uses_inline_storage();
    test_typed_exact_zero_uses_header_only();
    test_growth_spills_inline_to_heap_and_preserves_values();
    test_slice_marks_borrowed_storage();
    printf("test_xrt_array: %d passed, %d failed\n", g_passed, g_failed);
    return g_failed ? 1 : 0;
}
