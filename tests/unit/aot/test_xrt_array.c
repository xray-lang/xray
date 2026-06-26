#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <setjmp.h>

#define XRT_DATA_ALIGN 32

static int g_passed;
static int g_failed;
static int g_malloc_count;
static int g_free_count;
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
    if (ptr)
        g_free_count++;
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

// Emit the runtime impl (bump globals + allocator) into this TU using the custom
// XRT_* allocators above. Without it the static helpers in xrt_arc.h reference
// undefined externs whenever the compiler keeps an unused static (GCC at -O0/-O2
// on Linux); clang on macOS elided them, so the gap only showed under GCC.
#define XRT_IMPL
#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wundefined-internal"
#pragma clang diagnostic ignored "-Wunused-function"
#endif
#include "../../../src/aot/xrt_coll.h"
#if defined(__clang__)
#pragma clang diagnostic pop
#endif

static jmp_buf g_throw_jmp;
static int g_expect_throw;
static XrValue g_thrown_exc;

XRT_COLD _Noreturn void xrt_throw_exc(XrValue exc) {
    if (g_expect_throw) {
        g_thrown_exc = exc;
        longjmp(g_throw_jmp, 1);
    }
    fprintf(stderr, "unexpected xrt_throw_exc in test_xrt_array\n");
    abort();
}

static inline void xrt_dispatch_destructor(uint16_t type_id, void *obj) {
    (void) type_id;
    (void) obj;
}

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

#define ASSERT_XR_STR_EQ(actual, expected, msg)                                                    \
    do {                                                                                           \
        XrValue _actual = (actual);                                                                \
        const char *_expected = (expected);                                                        \
        size_t _expected_len = strlen(_expected);                                                  \
        if (!XR_IS_STR(_actual) || (size_t) xr_str_len(_actual) != _expected_len ||                \
            memcmp(xr_str_data(_actual), _expected, _expected_len) != 0) {                         \
            fprintf(stderr, "FAIL: %s (got '%.*s', expected '%s')\n", msg,                         \
                    XR_IS_STR(_actual) ? (int) xr_str_len(_actual) : 0,                            \
                    XR_IS_STR(_actual) ? xr_str_data(_actual) : "", _expected);                    \
            g_failed++;                                                                            \
            return;                                                                                \
        }                                                                                          \
        g_passed++;                                                                                \
    } while (0)

static void reset_alloc_counts(void) {
    g_malloc_count = 0;
    g_free_count = 0;
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
    ASSERT_EQ_INT(a->capacity, 4, "plain array minimum cap is 4");
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
    ASSERT_EQ_INT(a->capacity, 0, "exact zero typed array keeps cap 0");
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
    ASSERT_EQ_INT(a->capacity, 8, "first growth doubles cap");
    ASSERT_EQ_INT(g_aligned_alloc_count, 1, "first growth allocates one heap data buffer");
    ASSERT_EQ_INT(g_aligned_free_count, 0, "first growth does not free inline data");
    ASSERT_TRUE(ptr_is_aligned(a->data), "grown data is XRT_DATA_ALIGN-aligned");
    ASSERT_EQ_INT(((XrValue *) a->data)[0].i, 10, "first element survives inline spill");
    ASSERT_EQ_INT(((XrValue *) a->data)[4].i, 14, "new element stored after spill");

    for (int64_t i = 5; i < 9; i++)
        xrt_array_push(value, XR_FROM_INT(i + 10));

    ASSERT_TRUE(xrt_array_data_is_heap(a), "second growth remains heap storage");
    ASSERT_EQ_INT(a->capacity, 16, "second growth doubles cap");
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
    /* `.slice()` returns an independent copy (value semantics, matching the VM),
     * not a borrowed view, so mutating the slice never aliases the source. */
    ASSERT_TRUE(slice->data_storage != XR_ARRAY_DATA_BORROWED, "slice owns copied storage");
    ASSERT_TRUE(!xrt_array_data_is_borrowed(slice), "slice data is owned, not borrowed");
    ASSERT_TRUE((uint8_t *) slice->data != (uint8_t *) a->data + a->elem_size,
                "slice data does not alias source");
    ASSERT_EQ_INT(slice->length, 2, "slice has expected length");
    ASSERT_EQ_INT(((XrValue *) slice->data)[0].i, 1, "slice copies source data");

    free_test_array(slice);
    free_test_array(a);
}

static void test_slice_negative_bounds_and_aliasing(void) {
    reset_alloc_counts();
    XrValue value = xrt_array_new(5);
    xrt_array_t *a = (xrt_array_t *) value.ptr;
    for (int64_t i = 0; i < 5; i++)
        xrt_array_push(value, XR_FROM_INT(i + 10));

    XrValue slice_value = xrt_array_slice_view(value, -4, -1);
    xrt_array_t *slice = (xrt_array_t *) slice_value.ptr;
    ASSERT_TRUE(slice != NULL, "negative-bounds slice allocated");
    ASSERT_EQ_INT(slice->length, 3, "negative bounds produce expected count");
    ASSERT_EQ_INT(((XrValue *) slice->data)[0].i, 11, "negative start is from tail");
    /* Copy semantics: the slice owns its storage and does not alias the source. */
    ASSERT_TRUE(slice->data != (uint8_t *) a->data + a->elem_size,
                "slice data does not alias source offset");

    xr_typed_set(slice->data, 1, XR_FROM_INT(77), slice->elem_type);
    ASSERT_EQ_INT(((XrValue *) a->data)[2].i, 12, "slice write does not affect source");

    free_test_array(slice);
    free_test_array(a);
}

static void test_fill_range_typed_fast_path(void) {
    reset_alloc_counts();
    XrValue value = xrt_array_new_typed(0, XR_ELEM_I64);
    xrt_array_t *a = (xrt_array_t *) value.ptr;
    for (int64_t i = 0; i < 6; i++)
        xrt_array_push(value, XR_FROM_INT(i + 1));

    xrt_array_fill_range_value(value, XR_FROM_INT(99), -4, -1);
    ASSERT_EQ_INT(((int64_t *) a->data)[0], 1, "fill keeps prefix before negative range");
    ASSERT_EQ_INT(((int64_t *) a->data)[1], 2, "fill starts at tail-adjusted index");
    ASSERT_EQ_INT(((int64_t *) a->data)[2], 99, "fill writes first selected element");
    ASSERT_EQ_INT(((int64_t *) a->data)[4], 99, "fill writes end-exclusive range");
    ASSERT_EQ_INT(((int64_t *) a->data)[5], 6, "fill leaves suffix after end");

    xrt_array_fill_range_value(value, XR_FROM_INT(7), 5, 2);
    ASSERT_EQ_INT(((int64_t *) a->data)[2], 99, "empty fill leaves start untouched");
    ASSERT_EQ_INT(((int64_t *) a->data)[5], 6, "empty fill leaves end untouched");

    free_test_array(a);
}

static void test_resize_reserve_use_shared_capacity_plan(void) {
    reset_alloc_counts();
    XrValue value = xrt_array_new_typed(0, XR_ELEM_I64);
    xrt_array_t *a = (xrt_array_t *) value.ptr;
    for (int64_t i = 0; i < 3; i++)
        xrt_array_push(value, XR_FROM_INT(i + 1));

    int64_t initial_capacity = a->capacity;
    xrt_array_reserve_value(value, XR_FROM_INT(-9));
    ASSERT_EQ_INT(a->capacity, initial_capacity, "negative reserve is a no-op");
    xrt_array_reserve_value(value, XR_FROM_INT(9));
    ASSERT_TRUE(a->capacity >= 9, "reserve grows to requested capacity");
    ASSERT_EQ_INT(a->length, 3, "reserve does not change length");

    xrt_array_resize_value(value, XR_FROM_INT(6), XR_FROM_INT(42));
    ASSERT_EQ_INT(a->length, 6, "resize grow updates length");
    ASSERT_EQ_INT(((int64_t *) a->data)[3], 42, "resize fills first new slot");
    ASSERT_EQ_INT(((int64_t *) a->data)[5], 42, "resize fills last new slot");

    xrt_array_resize_value(value, XR_FROM_INT(-3), XR_FROM_INT(7));
    ASSERT_EQ_INT(a->length, 0, "negative resize clamps to zero length");

    free_test_array(a);
}

static void test_slice_resize_reserve_are_noops(void) {
    reset_alloc_counts();
    XrValue value = xrt_array_new_typed(0, XR_ELEM_I64);
    xrt_array_t *a = (xrt_array_t *) value.ptr;
    for (int64_t i = 0; i < 5; i++)
        xrt_array_push(value, XR_FROM_INT(i + 10));

    XrValue slice_value = xrt_array_slice_view(value, 1, 4);
    xrt_array_t *slice = (xrt_array_t *) slice_value.ptr;
    ASSERT_EQ_INT(slice->length, 3, "slice has expected initial length");
    /* The slice owns its copied storage, so resize/reserve operate on the copy
     * and never disturb the source array. */
    xrt_array_resize_value(slice_value, XR_FROM_INT(8), XR_FROM_INT(0));
    ASSERT_EQ_INT(slice->length, 8, "slice resize grow updates owned copy");
    ASSERT_EQ_INT(a->length, 5, "source length unaffected by slice resize");
    xrt_array_reserve_value(slice_value, XR_FROM_INT(12));
    ASSERT_TRUE(slice->capacity >= 12, "slice reserve grows owned copy");

    free_test_array(slice);
    free_test_array(a);
}

static void test_resize_reserve_type_errors_are_structured(void) {
    reset_alloc_counts();
    XrValue value = xrt_array_new_typed(0, XR_ELEM_I64);
    xrt_array_t *a = (xrt_array_t *) value.ptr;

    g_expect_throw = 1;
    if (setjmp(g_throw_jmp) == 0) {
        xrt_array_reserve_value(value, XR_TRUE_VAL);
        g_expect_throw = 0;
        ASSERT_TRUE(false, "reserve with non-int capacity throws");
    }
    g_expect_throw = 0;
    ASSERT_XR_STR_EQ(xrt_json_get_name(g_thrown_exc, "message"),
                     XR_ERROR_CORE_ARRAY_RESERVE_EXPECTS_MSG, "reserve type error message");
    XrValue code = xrt_json_get_name(g_thrown_exc, "code");
    ASSERT_TRUE(XR_IS_INT(code), "reserve type error code is int");
    ASSERT_EQ_INT(XR_TO_INT(code), XR_ERR_TYPE_MISMATCH, "reserve type error code");

    g_expect_throw = 1;
    if (setjmp(g_throw_jmp) == 0) {
        xrt_array_resize_value(value, XR_TRUE_VAL, XR_FROM_INT(0));
        g_expect_throw = 0;
        ASSERT_TRUE(false, "resize with non-int length throws");
    }
    g_expect_throw = 0;
    ASSERT_XR_STR_EQ(xrt_json_get_name(g_thrown_exc, "message"),
                     XR_ERROR_CORE_ARRAY_RESIZE_EXPECTS_MSG, "resize type error message");
    code = xrt_json_get_name(g_thrown_exc, "code");
    ASSERT_TRUE(XR_IS_INT(code), "resize type error code is int");
    ASSERT_EQ_INT(XR_TO_INT(code), XR_ERR_TYPE_MISMATCH, "resize type error code");

    free_test_array(a);
}

static void test_indexof_typed_fast_path_shared_rules(void) {
    reset_alloc_counts();
    XrValue bytes = xrt_array_new_typed(0, XR_ELEM_U8);
    xrt_array_t *b = (xrt_array_t *) bytes.ptr;
    xrt_array_push(bytes, XR_FROM_INT(1));
    xrt_array_push(bytes, XR_FROM_INT(255));
    xrt_array_push(bytes, XR_FROM_INT(3));

    int handled = 0;
    ASSERT_EQ_INT(xrt_array_indexof_typed_fast(b, XR_FROM_INT(255), &handled), 1,
                  "u8 search finds integer needle");
    ASSERT_EQ_INT(handled, 1, "u8 search handled by shared core");
    ASSERT_EQ_INT(xrt_array_indexof_typed_fast(b, XR_FROM_INT(-1), &handled), -1,
                  "u8 search rejects out-of-range integer");
    ASSERT_EQ_INT(xrt_array_indexof_typed_fast(b, XR_FROM_BOOL(1), &handled), -1,
                  "u8 search rejects bool needle");

    XrValue bools = xrt_array_new_typed(0, XR_ELEM_BOOL);
    xrt_array_t *flags = (xrt_array_t *) bools.ptr;
    xrt_array_push(bools, XR_FROM_BOOL(0));
    xrt_array_push(bools, XR_FROM_BOOL(1));
    ASSERT_EQ_INT(xrt_array_indexof_typed_fast(flags, XR_FROM_BOOL(1), &handled), 1,
                  "bool search finds bool needle");
    ASSERT_EQ_INT(xrt_array_indexof_typed_fast(flags, XR_FROM_INT(1), &handled), -1,
                  "bool search rejects integer needle");

    free_test_array(b);
    free_test_array(flags);
}

static void test_bytes_raw_helpers_share_core_rules(void) {
    reset_alloc_counts();
    XrValue value = xrt_array_new_typed(0, XR_ELEM_U8);
    xrt_array_t *a = (xrt_array_t *) value.ptr;
    for (int64_t i = 1; i <= 8; i++)
        xrt_array_push(value, XR_FROM_INT(i));

    ASSERT_EQ_INT(xrt_bytes_load_u32_le_raw(a, 0), 67305985, "u32 load is little-endian");
    ASSERT_EQ_INT((int64_t) xrt_bytes_load_u64_le_raw(a, 0), 578437695752307201LL,
                  "u64 load is little-endian");
    xrt_bytes_copy_within_raw(a, 2, 0, 4);
    ASSERT_EQ_INT(((uint8_t *) a->data)[2], 1, "copyWithin writes first overlap byte");
    ASSERT_EQ_INT(((uint8_t *) a->data)[3], 2, "copyWithin writes second overlap byte");
    ASSERT_EQ_INT(((uint8_t *) a->data)[5], 4, "copyWithin writes last selected byte");

    XrValue dst_value = xrt_array_new_typed(0, XR_ELEM_U8);
    xrt_array_t *dst = (xrt_array_t *) dst_value.ptr;
    for (int64_t i = 0; i < 6; i++)
        xrt_array_push(dst_value, XR_FROM_INT(0));
    xrt_bytes_copy_from_raw(dst, a, 1, 2, 3);
    ASSERT_EQ_INT(((uint8_t *) dst->data)[2], 2, "copyFrom writes first source byte");
    ASSERT_EQ_INT(((uint8_t *) dst->data)[3], 1, "copyFrom preserves shared source state");
    ASSERT_EQ_INT(((uint8_t *) dst->data)[4], 2, "copyFrom writes count bytes");
    XrValue rep_value = xrt_array_new_typed(0, XR_ELEM_U8);
    xrt_array_t *rep = (xrt_array_t *) rep_value.ptr;
    uint8_t seed[] = {65, 66, 67, 0, 0, 0, 0, 0, 0};
    for (int64_t i = 0; i < 9; i++)
        xrt_array_push(rep_value, XR_FROM_INT(seed[i]));
    xrt_bytes_repeat_from_raw(rep, 3, 3, 6);
    ASSERT_EQ_INT(((uint8_t *) rep->data)[3], 65, "repeatFrom writes first repeat byte");
    ASSERT_EQ_INT(((uint8_t *) rep->data)[4], 66, "repeatFrom writes second repeat byte");
    ASSERT_EQ_INT(((uint8_t *) rep->data)[5], 67, "repeatFrom writes third repeat byte");
    ASSERT_EQ_INT(((uint8_t *) rep->data)[8], 67, "repeatFrom repeats through overlap");
    free_test_array(a);
    free_test_array(dst);
    free_test_array(rep);
}

static XrValue dummy_closure_body(xrt_closure_t *cl) {
    (void) cl;
    return XR_NULL_VAL;
}

static void test_stack_closure_borrows_cell_upval(void) {
    reset_alloc_counts();
    XrValue arr = xrt_array_new_typed(0, XR_ELEM_I64);
    XrValue cell = xrt_cell_new(XR_NULL_VAL);
    xrt_cell_set(cell, arr);

    XrValue closure = xrt_closure_stack_new((void *) dummy_closure_body, 1);
    ((xrt_closure_t *) closure.ptr)->upvals[0] = cell;
    xrt_release(closure);

    xrt_array_push(arr, XR_FROM_INT(42));
    xrt_array_t *a = (xrt_array_t *) arr.ptr;
    ASSERT_EQ_INT(a->length, 1, "stack closure release must not destroy borrowed cell");
    ASSERT_EQ_INT(((int64_t *) a->data)[0], 42, "array remains writable through local owner");

    xrt_release(arr);
    xrt_release(cell);
}

int main(void) {
    test_small_array_uses_inline_storage();
    test_typed_exact_zero_uses_header_only();
    test_growth_spills_inline_to_heap_and_preserves_values();
    test_slice_marks_borrowed_storage();
    test_slice_negative_bounds_and_aliasing();
    test_fill_range_typed_fast_path();
    test_resize_reserve_use_shared_capacity_plan();
    test_slice_resize_reserve_are_noops();
    test_resize_reserve_type_errors_are_structured();
    test_indexof_typed_fast_path_shared_rules();
    test_bytes_raw_helpers_share_core_rules();
    test_stack_closure_borrows_cell_upval();
    printf("test_xrt_array: %d passed, %d failed\n", g_passed, g_failed);
    return g_failed ? 1 : 0;
}
