#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define XRT_DATA_ALIGN 32

static int g_passed;
static int g_failed;

static void *test_malloc(size_t size) {
    return malloc(size);
}

static void *test_calloc(size_t count, size_t size) {
    return calloc(count, size);
}

static void *test_realloc(void *ptr, size_t size) {
    return realloc(ptr, size);
}

static void test_free(void *ptr) {
    free(ptr);
}

static void *test_alloc_aligned(size_t size) {
    void *ptr = NULL;
    return posix_memalign(&ptr, XRT_DATA_ALIGN, size) == 0 ? ptr : NULL;
}

static void test_free_aligned(void *ptr) {
    free(ptr);
}

#define XRT_MALLOC(sz) test_malloc(sz)
#define XRT_CALLOC(n, sz) test_calloc((n), (sz))
#define XRT_REALLOC(p, sz) test_realloc((p), (sz))
#define XRT_FREE(p) test_free(p)
#define XRT_ALLOC_ALIGNED(sz) test_alloc_aligned(sz)
#define XRT_FREE_ALIGNED(p) test_free_aligned(p)

#define XRT_IMPL
#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wundefined-internal"
#pragma clang diagnostic ignored "-Wunused-function"
#endif
#include "../../../src/aot/xrt_method.h"
#if defined(__clang__)
#pragma clang diagnostic pop
#endif

XRT_COLD _Noreturn void xrt_throw_exc(XrValue exc) {
    (void) exc;
    fprintf(stderr, "unexpected xrt_throw_exc in test_xrt_method_truthy\n");
    abort();
}

static inline void xrt_dispatch_destructor(uint16_t type_id, void *obj) {
    (void) type_id;
    (void) obj;
}

#define ASSERT_BOOL(value, expected, msg)                                                          \
    do {                                                                                           \
        XrValue _actual = (value);                                                                 \
        if (_actual.tag != XR_TAG_BOOL || ((_actual.i != 0) != (expected))) {                      \
            fprintf(stderr, "FAIL: %s (got tag %d value %lld, expected %s)\n", msg, _actual.tag,   \
                    (long long) _actual.i, (expected) ? "true" : "false");                         \
            g_failed++;                                                                            \
            return;                                                                                \
        }                                                                                          \
        g_passed++;                                                                                \
    } while (0)

#define ASSERT_WEAK_ACCEPTS(value, expected, msg)                                                  \
    do {                                                                                           \
        int _actual = xrt_weak_value_is_heap_object(value);                                        \
        if ((_actual != 0) != (expected)) {                                                        \
            fprintf(stderr, "FAIL: %s (got %d, expected %s)\n", msg, _actual,                      \
                    (expected) ? "accepted" : "rejected");                                         \
            g_failed++;                                                                            \
            return;                                                                                \
        }                                                                                          \
        g_passed++;                                                                                \
    } while (0)

static XrValue test_string_with_bytes(const char *bytes, size_t len) {
    XrValue s = xrt_str_alloc(len);
    if (len != 0)
        memcpy(xr_str_buf(s), bytes, len);
    xr_str_buf(s)[len] = '\0';
    return s;
}

static void test_xrt_to_bool_reuses_truthy_core_for_scalars_and_strings(void) {
    ASSERT_BOOL(xrt_to_bool(XR_NULL_VAL), false, "null is falsy");
    ASSERT_BOOL(xrt_to_bool(XR_FALSE_VAL), false, "false is falsy");
    ASSERT_BOOL(xrt_to_bool(XR_TRUE_VAL), true, "true is truthy");
    ASSERT_BOOL(xrt_to_bool(XR_FROM_INT(0)), false, "zero int is falsy");
    ASSERT_BOOL(xrt_to_bool(XR_FROM_INT(-1)), true, "nonzero int is truthy");
    ASSERT_BOOL(xrt_to_bool(XR_FROM_FLOAT(0.0)), false, "zero float is falsy");
    ASSERT_BOOL(xrt_to_bool(XR_FROM_FLOAT(-0.25)), true, "nonzero float is truthy");

    XrValue empty = xrt_str_alloc(0);
    ASSERT_BOOL(xrt_to_bool(empty), false, "empty string is falsy");

    const char nul_first[] = {'\0'};
    XrValue nul_string = test_string_with_bytes(nul_first, sizeof(nul_first));
    ASSERT_BOOL(xrt_to_bool(nul_string), true, "nonempty string uses length, not first byte");

    XrValue text = test_string_with_bytes("xray", 4);
    ASSERT_BOOL(xrt_to_bool(text), true, "nonempty string is truthy");
}

static void test_xrt_to_bool_reuses_truthy_core_for_sized_containers(void) {
    XrValue arr = xrt_array_new(0);
    ASSERT_BOOL(xrt_to_bool(arr), false, "empty array is falsy");
    xrt_array_push(arr, XR_FROM_INT(1));
    ASSERT_BOOL(xrt_to_bool(arr), true, "nonempty array is truthy");

    XrValue map = xrt_map_new(0);
    ASSERT_BOOL(xrt_to_bool(map), false, "empty map is falsy");
    xrt_map_set((xrt_map_t *) map.ptr, xr_box_str("key"), XR_FROM_INT(1));
    ASSERT_BOOL(xrt_to_bool(map), true, "nonempty map is truthy");

    XrValue set = xrt_set_new(0);
    ASSERT_BOOL(xrt_to_bool(set), false, "empty set is falsy");
    xrt_set_add((xrt_set_t *) set.ptr, XR_FROM_INT(1));
    ASSERT_BOOL(xrt_to_bool(set), true, "nonempty set is truthy");
}

static void test_xrt_weak_predicate_accepts_aot_object_tags(void) {
    char dummy = 0;
    XrAotEnumValueView enum_view = {0};

    ASSERT_WEAK_ACCEPTS(XR_NULL_VAL, false, "null is not a weak key object");
    ASSERT_WEAK_ACCEPTS(XR_FROM_INT(1), false, "int is not a weak key object");
    ASSERT_WEAK_ACCEPTS(XR_TRUE_VAL, false, "bool is not a weak key object");
    ASSERT_WEAK_ACCEPTS(xrt_range_from_i64(1, 4, false), true, "Range is an object-like weak key");
    ASSERT_WEAK_ACCEPTS(xr_mkptr(&enum_view, XR_TAG_ENUM), true,
                        "Enum value view is an object-like weak key");
    ASSERT_WEAK_ACCEPTS(xr_mkptr(&dummy, XR_TAG_ITERATOR), true,
                        "Iterator is an object-like weak key");
    ASSERT_WEAK_ACCEPTS(xr_struct_ref(&dummy, 1), true,
                        "native struct reference is an object-like weak key");
}

int main(void) {
    test_xrt_to_bool_reuses_truthy_core_for_scalars_and_strings();
    test_xrt_to_bool_reuses_truthy_core_for_sized_containers();
    test_xrt_weak_predicate_accepts_aot_object_tags();

    if (g_failed == 0) {
        printf("test_xrt_method_truthy: %d passed, %d failed\n", g_passed, g_failed);
        return 0;
    }
    printf("test_xrt_method_truthy: %d passed, %d failed\n", g_passed, g_failed);
    return 1;
}
