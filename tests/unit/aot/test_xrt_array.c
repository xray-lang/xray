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

#define EXPECT_XRT_ERROR_THROW(stmt, expected_code, expected_message, msg)                         \
    do {                                                                                           \
        g_thrown_exc = XR_NULL_VAL;                                                                \
        g_expect_throw = 1;                                                                        \
        if (setjmp(g_throw_jmp) == 0) {                                                            \
            stmt;                                                                                  \
            g_expect_throw = 0;                                                                    \
            ASSERT_TRUE(false, msg);                                                               \
        }                                                                                          \
        g_expect_throw = 0;                                                                        \
        XrValue _code = xrt_json_get_name(g_thrown_exc, "code");                                   \
        ASSERT_TRUE(XR_IS_INT(_code), msg " code is int");                                         \
        ASSERT_EQ_INT(XR_TO_INT(_code), expected_code, msg " code");                               \
        ASSERT_XR_STR_EQ(xrt_json_get_name(g_thrown_exc, "message"), expected_message,             \
                         msg " message");                                                          \
    } while (0)

#define EXPECT_READONLY_BYTE_SLICE_THROW(stmt, msg)                                                \
    do {                                                                                           \
        EXPECT_XRT_ERROR_THROW(stmt, XR_ERR_CMP_CONST_ASSIGN,                                      \
                               XR_ERROR_CORE_BYTE_SLICE_READONLY_MSG, msg);                        \
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
    ASSERT_EQ_INT(a->length, 2, "plain array constructor sets logical length");
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
    XrValue value = xrt_array_new(0);
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
    XrValue value = xrt_array_new(0);
    xrt_array_t *a = (xrt_array_t *) value.ptr;
    for (int64_t i = 0; i < 4; i++)
        xrt_array_push(value, XR_FROM_INT(i));

    XrValue slice_value = xrt_array_slice_view(value, 1, 3);
    xrt_array_t *slice = (xrt_array_t *) slice_value.ptr;
    ASSERT_TRUE(slice != NULL, "slice allocated");
    ASSERT_TRUE(slice->data_storage == XR_ARRAY_DATA_BORROWED, "slice borrows source storage");
    ASSERT_TRUE(xrt_array_data_is_borrowed(slice), "slice data is borrowed");
    ASSERT_TRUE((uint8_t *) slice->data == (uint8_t *) a->data + a->elem_size,
                "slice data aliases source offset");
    ASSERT_EQ_INT(slice->length, 2, "slice has expected length");
    ASSERT_EQ_INT(((XrValue *) slice->data)[0].i, 1, "slice reads source data");
    xr_typed_set(slice->data, 0, XR_FROM_INT(77), slice->elem_type);
    ASSERT_EQ_INT(((XrValue *) a->data)[1].i, 77, "slice write affects source");

    free_test_array(slice);
    free_test_array(a);
}

static void test_slice_negative_bounds_and_aliasing(void) {
    reset_alloc_counts();
    XrValue value = xrt_array_new(0);
    xrt_array_t *a = (xrt_array_t *) value.ptr;
    for (int64_t i = 0; i < 5; i++)
        xrt_array_push(value, XR_FROM_INT(i + 10));

    XrValue slice_value = xrt_array_slice_view(value, -4, -1);
    xrt_array_t *slice = (xrt_array_t *) slice_value.ptr;
    ASSERT_TRUE(slice != NULL, "negative-bounds slice allocated");
    ASSERT_EQ_INT(slice->length, 3, "negative bounds produce expected count");
    ASSERT_EQ_INT(((XrValue *) slice->data)[0].i, 11, "negative start is from tail");
    ASSERT_TRUE(slice->data == (uint8_t *) a->data + a->elem_size,
                "slice data aliases source offset");

    xr_typed_set(slice->data, 1, XR_FROM_INT(77), slice->elem_type);
    ASSERT_EQ_INT(((XrValue *) a->data)[2].i, 77, "slice write affects source");

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

static void test_typed_filled_constructor_uses_pod_storage_rules(void) {
    reset_alloc_counts();
    XrValue zeros_value = xrt_array_new_filled_value(XR_FROM_INT(5), XR_FROM_INT(0), XR_ELEM_U32);
    xrt_array_t *zeros = (xrt_array_t *) zeros_value.ptr;

    ASSERT_EQ_INT(zeros->length, 5, "zero-filled u32 constructor sets logical length");
    ASSERT_EQ_INT(zeros->capacity, 5, "zero-filled u32 constructor keeps requested capacity");
    ASSERT_EQ_INT(((uint32_t *) zeros->data)[0], 0, "zero-filled u32 first slot is zero");
    ASSERT_EQ_INT(((uint32_t *) zeros->data)[4], 0, "zero-filled u32 last slot is zero");

    XrValue fill_value =
        xrt_array_new_filled_value(XR_FROM_INT(3), XR_FROM_INT(0x12345678), XR_ELEM_U32);
    xrt_array_t *filled = (xrt_array_t *) fill_value.ptr;
    ASSERT_EQ_INT(filled->length, 3, "non-zero u32 filled constructor sets logical length");
    ASSERT_EQ_INT(((uint32_t *) filled->data)[0], 0x12345678,
                  "non-zero u32 filled constructor writes first slot");
    ASSERT_EQ_INT(((uint32_t *) filled->data)[2], 0x12345678,
                  "non-zero u32 filled constructor writes last slot");

    XrValue flags_value = xrt_array_new_filled_value(XR_FROM_INT(4), XR_TRUE_VAL, XR_ELEM_BOOL);
    xrt_array_t *flags = (xrt_array_t *) flags_value.ptr;
    ASSERT_EQ_INT(flags->length, 4, "bool filled constructor sets logical length");
    ASSERT_EQ_INT(((uint8_t *) flags->data)[0], 1, "bool filled constructor writes first slot");
    ASSERT_EQ_INT(((uint8_t *) flags->data)[3], 1, "bool filled constructor writes last slot");

    free_test_array(zeros);
    free_test_array(filled);
    free_test_array(flags);
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
    xrt_array_resize_value(slice_value, XR_FROM_INT(8), XR_FROM_INT(0));
    ASSERT_EQ_INT(slice->length, 3, "slice resize leaves view length unchanged");
    ASSERT_EQ_INT(a->length, 5, "source length unaffected by slice resize");
    xrt_array_reserve_value(slice_value, XR_FROM_INT(12));
    ASSERT_EQ_INT(slice->capacity, 3, "slice reserve leaves view capacity unchanged");

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

static void test_byte_array_raw_helpers_share_core_rules(void) {
    reset_alloc_counts();
    XrValue value = xrt_array_new_typed(0, XR_ELEM_U8);
    xrt_array_t *a = (xrt_array_t *) value.ptr;
    for (int64_t i = 1; i <= 8; i++)
        xrt_array_push(value, XR_FROM_INT(i));

    ASSERT_EQ_INT(xrt_byte_array_load_u16_le_raw(a, 0), 513, "u16 load is little-endian");
    ASSERT_EQ_INT(xrt_byte_array_load_u32_le_raw(a, 0), 67305985, "u32 load is little-endian");
    ASSERT_EQ_INT((int64_t) xrt_byte_array_load_u64_le_raw(a, 0), 578437695752307201LL,
                  "u64 load is little-endian");
    const uint8_t *raw = (const uint8_t *) a->data;
    ASSERT_EQ_INT(xrt_ptr_load_u16_le_unchecked_raw(raw + 1), 770,
                  "raw pointer u16 load is little-endian");
    ASSERT_EQ_INT(xrt_ptr_load_u32_le_unchecked_raw(raw + 1), 84148994,
                  "raw pointer u32 load is little-endian");
    ASSERT_EQ_INT((int64_t) xrt_ptr_load_u64_le_unchecked_raw(raw), 578437695752307201LL,
                  "raw pointer u64 load is little-endian");
    xrt_byte_array_copy_within_raw(a, 2, 0, 4);
    ASSERT_EQ_INT(((uint8_t *) a->data)[2], 1, "copyWithin writes first overlap byte");
    ASSERT_EQ_INT(((uint8_t *) a->data)[3], 2, "copyWithin writes second overlap byte");
    ASSERT_EQ_INT(((uint8_t *) a->data)[5], 4, "copyWithin writes last selected byte");

    XrValue dst_value = xrt_array_new_typed(0, XR_ELEM_U8);
    xrt_array_t *dst = (xrt_array_t *) dst_value.ptr;
    for (int64_t i = 0; i < 6; i++)
        xrt_array_push(dst_value, XR_FROM_INT(0));
    xrt_byte_array_copy_from_raw(dst, a, 1, 2, 3);
    ASSERT_EQ_INT(((uint8_t *) dst->data)[2], 2, "copyFrom writes first source byte");
    ASSERT_EQ_INT(((uint8_t *) dst->data)[3], 1, "copyFrom preserves shared source state");
    ASSERT_EQ_INT(((uint8_t *) dst->data)[4], 2, "copyFrom writes count bytes");

    EXPECT_XRT_ERROR_THROW(xrt_byte_array_copy_within_checked_raw(a, 7, 0, 2),
                           XR_ERR_INDEX_OUT_OF_BOUNDS, XR_ERROR_CORE_BYTES_COPY_WITHIN_OOB_MSG,
                           "Array<byte> copy-within checked helper throws on range");
    XrValue int_arr_value = xrt_array_new_typed_exact(0, XR_ELEM_I64);
    xrt_array_t *int_arr = (xrt_array_t *) int_arr_value.ptr;
    EXPECT_XRT_ERROR_THROW(xrt_byte_array_copy_from_checked_raw(dst, int_arr, 0, 0, 1),
                           XR_ERR_TYPE_MISMATCH, XR_ERROR_CORE_BYTES_COPY_FROM_OPERANDS_MSG,
                           "Array<byte> copy range checked helper throws on typed operand");
    EXPECT_XRT_ERROR_THROW(xrt_byte_array_copy_from_value(dst_value, value, XR_NULL_VAL,
                                                          XR_FROM_INT(0), XR_FROM_INT(1)),
                           XR_ERR_TYPE_MISMATCH, XR_ERROR_CORE_BYTES_COPY_FROM_EXPECTS_MSG,
                           "Array<byte> copy range value helper rejects non-integer offset");
    EXPECT_XRT_ERROR_THROW(xrt_byte_array_copy_from_value(dst_value, value, XR_FROM_INT(100),
                                                          XR_FROM_INT(0), XR_FROM_INT(1)),
                           XR_ERR_INDEX_OUT_OF_BOUNDS, XR_ERROR_CORE_BYTES_COPY_FROM_OOB_MSG,
                           "Array<byte> copy range value helper throws on range");

    XrValue rep_value = xrt_array_new_typed(0, XR_ELEM_U8);
    xrt_array_t *rep = (xrt_array_t *) rep_value.ptr;
    uint8_t seed[] = {65, 66, 67, 0, 0, 0, 0, 0, 0};
    for (int64_t i = 0; i < 9; i++)
        xrt_array_push(rep_value, XR_FROM_INT(seed[i]));
    xrt_byte_array_repeat_from_raw(rep, 3, 3, 6);
    ASSERT_EQ_INT(((uint8_t *) rep->data)[3], 65, "repeatFrom writes first repeat byte");
    ASSERT_EQ_INT(((uint8_t *) rep->data)[4], 66, "repeatFrom writes second repeat byte");
    ASSERT_EQ_INT(((uint8_t *) rep->data)[5], 67, "repeatFrom writes third repeat byte");
    ASSERT_EQ_INT(((uint8_t *) rep->data)[8], 67, "repeatFrom repeats through overlap");

    XrValue safe_append_value = xrt_array_new_typed_exact(2, XR_ELEM_U8);
    xrt_array_t *safe_append = (xrt_array_t *) safe_append_value.ptr;
    xrt_array_push(safe_append_value, XR_FROM_INT(65));
    xrt_array_push(safe_append_value, XR_FROM_INT(66));
    xr_span_t safe_src = xrt_span_from_array_slice(safe_append_value, 0, 2);
    xrt_byte_array_append_from_span_raw(safe_append, safe_src);
    ASSERT_EQ_INT(safe_append->length, 4, "appendFrom grows and commits length");
    ASSERT_EQ_INT(((uint8_t *) safe_append->data)[2], 65,
                  "appendFrom keeps aliased source valid across grow");
    ASSERT_EQ_INT(((uint8_t *) safe_append->data)[3], 66,
                  "appendFrom copies aliased source after grow");
    xrt_byte_array_repeat_from_tail_raw(safe_append, 2, 4);
    ASSERT_EQ_INT(safe_append->length, 8, "repeatFrom grows and commits repeated tail");
    ASSERT_EQ_INT(((uint8_t *) safe_append->data)[6], 65,
                  "repeatFrom repeats first source byte at tail");
    ASSERT_EQ_INT(((uint8_t *) safe_append->data)[7], 66,
                  "repeatFrom repeats second source byte at tail");

    XrValue span_ops_value = xrt_array_new_typed_exact(16, XR_ELEM_U8);
    xrt_array_t *span_ops = (xrt_array_t *) span_ops_value.ptr;
    for (int64_t i = 0; i < 12; i++)
        xrt_array_push(span_ops_value, XR_FROM_INT(65 + i));
    xr_span_t span_all = xrt_span_from_array_slice(span_ops_value, 0, 12);
    xrt_byte_slice_repeat_from_checked_raw(span_all, 4, 4, 4);
    ASSERT_EQ_INT(((uint8_t *) span_ops->data)[4], 65,
                  "Slice<byte>.repeatFrom writes first repeated byte");
    ASSERT_EQ_INT(((uint8_t *) span_ops->data)[7], 68,
                  "Slice<byte>.repeatFrom writes through overlap");
    xr_span_t copy_dst = xrt_span_from_array_slice(span_ops_value, 8, 12);
    xr_span_t copy_src = xrt_span_from_array_slice(span_ops_value, 4, 8);
    xrt_byte_slice_copy_checked_raw(copy_dst, copy_src);
    ASSERT_EQ_INT(((uint8_t *) span_ops->data)[8], 65,
                  "Slice<byte>.copyFrom writes first source byte");
    ASSERT_EQ_INT(((uint8_t *) span_ops->data)[11], 68,
                  "Slice<byte>.copyFrom writes the final source byte");
    ASSERT_EQ_INT(
        xrt_byte_slice_common_prefix_checked_raw(xrt_span_from_array_slice(span_ops_value, 0, 4),
                                                 xrt_span_from_array_slice(span_ops_value, 8, 12)),
        4, "Slice<byte>.commonPrefix compares safe span slices");

    free_test_array(a);
    free_test_array(dst);
    free_test_array(int_arr);
    free_test_array(rep);
    free_test_array(span_ops);
}

static void test_byte_runtime_u8_guards_are_defensive(void) {
    XrValue bytes_value = xrt_array_new_typed(0, XR_ELEM_U8);
    xrt_array_t *bytes = (xrt_array_t *) bytes_value.ptr;
    for (int64_t i = 0; i < 4; i++)
        xrt_array_push(bytes_value, XR_FROM_INT(10 + i));

    XrValue int_value = xrt_array_new_typed(4, XR_ELEM_I64);
    xrt_array_t *ints = (xrt_array_t *) int_value.ptr;

    EXPECT_XRT_ERROR_THROW(xrt_byte_array_load_u32_le(int_value, XR_FROM_INT(0)),
                           XR_ERR_TYPE_MISMATCH, XR_ERROR_CORE_BYTES_LOAD_U32_RECEIVER_MSG,
                           "AOT defensive byte load rejects non-U8 receiver");
    EXPECT_XRT_ERROR_THROW(
        xrt_byte_array_copy_within_value(int_value, XR_FROM_INT(0), XR_FROM_INT(0), XR_FROM_INT(1)),
        XR_ERR_TYPE_MISMATCH, XR_ERROR_CORE_BYTES_COPY_WITHIN_RECEIVER_MSG,
        "AOT defensive byte copy-within rejects non-U8 receiver");
    EXPECT_XRT_ERROR_THROW(xrt_byte_array_copy_from_value(int_value, bytes_value, XR_FROM_INT(0),
                                                          XR_FROM_INT(0), XR_FROM_INT(1)),
                           XR_ERR_TYPE_MISMATCH, XR_ERROR_CORE_BYTES_COPY_FROM_OPERANDS_MSG,
                           "AOT defensive byte copy-from rejects non-U8 destination");
    EXPECT_XRT_ERROR_THROW(xrt_byte_array_append_from_value(int_value, bytes_value),
                           XR_ERR_TYPE_MISMATCH, XR_ERROR_CORE_BYTES_APPEND_FROM_OPERANDS_MSG,
                           "AOT defensive appendFrom rejects non-U8 receiver");
    EXPECT_XRT_ERROR_THROW(
        xrt_byte_array_repeat_from_tail_value(int_value, XR_FROM_INT(1), XR_FROM_INT(1)),
        XR_ERR_TYPE_MISMATCH, XR_ERROR_CORE_BYTES_REPEAT_FROM_RECEIVER_MSG,
        "AOT defensive repeatFrom rejects non-U8 receiver");
    EXPECT_XRT_ERROR_THROW(xrt_byte_array_repeat_from_value(XR_FROM_INT(0), XR_FROM_INT(0),
                                                            XR_FROM_INT(1), XR_FROM_INT(1)),
                           XR_ERR_TYPE_MISMATCH, XR_ERROR_CORE_BYTES_REPEAT_FROM_RECEIVER_MSG,
                           "AOT defensive repeat op rejects non-array receiver");

    ASSERT_EQ_INT(bytes->elem_type, XR_ELEM_U8, "valid byte storage remains U8");
    ASSERT_EQ_INT(ints->elem_type, XR_ELEM_I64, "invalid test receiver remains non-U8");

    free_test_array(bytes);
    free_test_array(ints);
}

static void test_byte_slice_readonly_mutators_throw_before_write(void) {
    reset_alloc_counts();
    XrValue value = xrt_array_new_typed_exact(8, XR_ELEM_U8);
    xrt_array_t *a = (xrt_array_t *) value.ptr;
    for (int64_t i = 0; i < 8; i++)
        xrt_array_push(value, XR_FROM_INT(10 + i));

    xr_span_t readonly = xrt_span_from_array_slice(value, 0, 8);
    readonly.flags |= XRT_SPAN_FLAG_READONLY;
    xr_span_t src = xrt_span_from_array_slice(value, 0, 4);

    EXPECT_READONLY_BYTE_SLICE_THROW(xrt_byte_slice_fill_checked_raw(readonly, 0xff),
                                     "readonly Slice<byte>.fill throws");
    EXPECT_READONLY_BYTE_SLICE_THROW(xrt_byte_slice_copy_checked_raw(readonly, src),
                                     "readonly Slice<byte>.copyFrom throws");
    EXPECT_READONLY_BYTE_SLICE_THROW(xrt_byte_slice_repeat_from_checked_raw(readonly, 4, 4, 4),
                                     "readonly Slice<byte>.repeatFrom throws");
    EXPECT_READONLY_BYTE_SLICE_THROW(xrt_byte_slice_store_u16_checked_raw(readonly, 0, 0xffff, 1),
                                     "readonly Slice<byte>.store throws");

    for (int64_t i = 0; i < 8; i++)
        ASSERT_EQ_INT(((uint8_t *) a->data)[i], 10 + i,
                      "readonly mutator guards leave byte storage unchanged");

    free_test_array(a);
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
    test_typed_filled_constructor_uses_pod_storage_rules();
    test_resize_reserve_use_shared_capacity_plan();
    test_slice_resize_reserve_are_noops();
    test_resize_reserve_type_errors_are_structured();
    test_indexof_typed_fast_path_shared_rules();
    test_byte_array_raw_helpers_share_core_rules();
    test_byte_runtime_u8_guards_are_defensive();
    test_byte_slice_readonly_mutators_throw_before_write();
    test_stack_closure_borrows_cell_upval();
    printf("test_xrt_array: %d passed, %d failed\n", g_passed, g_failed);
    return g_failed ? 1 : 0;
}
