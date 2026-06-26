#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

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
#include "../../../src/aot/xrt_coll.h"
#if defined(__clang__)
#pragma clang diagnostic pop
#endif

XRT_COLD _Noreturn void xrt_throw_exc(XrValue exc) {
    (void) exc;
    fprintf(stderr, "unexpected xrt_throw_exc in test_xrt_boolmap\n");
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

#define ASSERT_EQ_TAG(actual, expected, msg)                                                       \
    do {                                                                                           \
        if ((actual).tag != (expected)) {                                                          \
            fprintf(stderr, "FAIL: %s (got tag %d, expected %d)\n", msg, (actual).tag,             \
                    (expected));                                                                   \
            g_failed++;                                                                            \
            return;                                                                                \
        }                                                                                          \
        g_passed++;                                                                                \
    } while (0)

static void test_boolmap_tagged_bool_keys_only(void) {
    XrValue mv = xrt_boolmap_new_typed(0, XR_ELEM_I64);
    xrt_boolmap_t *m = (xrt_boolmap_t *) mv.ptr;

    xrt_boolmap_set_v(m, XR_FROM_BOOL(false), XR_FROM_INT(34));
    xrt_boolmap_set_v(m, XR_FROM_BOOL(true), XR_FROM_INT(21));

    ASSERT_EQ_INT(xrt_boolmap_len(m), 2, "boolmap stores both bool keys");
    ASSERT_EQ_INT(xrt_boolmap_get_v(m, XR_FROM_BOOL(false)).i, 34, "false key get");
    ASSERT_EQ_INT(xrt_boolmap_get_v(m, XR_FROM_BOOL(true)).i, 21, "true key get");
    ASSERT_TRUE(xrt_boolmap_has_v(m, XR_FROM_BOOL(false)), "false key has");
    ASSERT_TRUE(xrt_boolmap_has_v(m, XR_FROM_BOOL(true)), "true key has");

    ASSERT_EQ_TAG(xrt_boolmap_get_v(m, XR_FROM_INT(0)), XR_TAG_NULL,
                  "int 0 must not alias bool false");
    ASSERT_EQ_TAG(xrt_boolmap_get_v(m, XR_FROM_INT(1)), XR_TAG_NULL,
                  "int 1 must not alias bool true");
    ASSERT_TRUE(!xrt_boolmap_has_v(m, XR_FROM_INT(1)), "int key must not report present");
    ASSERT_TRUE(!xrt_boolmap_delete_v(m, XR_FROM_INT(0)), "int key must not delete false slot");
    ASSERT_EQ_INT(xrt_boolmap_len(m), 2, "wrong-tag delete leaves boolmap untouched");
    xrt_boolmap_set_v(m, XR_FROM_INT(1), XR_FROM_INT(99));
    ASSERT_EQ_INT(xrt_boolmap_get_v(m, XR_FROM_BOOL(true)).i, 21,
                  "wrong-tag set must not overwrite true slot");
    ASSERT_EQ_INT(xrt_boolmap_len(m), 2, "wrong-tag set leaves boolmap length untouched");

    xrt_boolmap_destroy(m);
}

static void test_boolmap_f32_tagged_value_still_narrows(void) {
    XrValue mv = xrt_boolmap_new_typed(0, XR_ELEM_F32);
    xrt_boolmap_t *m = (xrt_boolmap_t *) mv.ptr;

    xrt_boolmap_set_v(m, XR_FROM_BOOL(true), XR_FROM_FLOAT(1.25));
    XrValue got = xrt_boolmap_get_v(m, XR_FROM_BOOL(true));

    ASSERT_EQ_TAG(got, XR_TAG_F64, "float32 boolmap get boxes as float");
    ASSERT_TRUE(fabs(got.f - 1.25) < 0.00001, "float32 boolmap value round-trips");

    xrt_boolmap_destroy(m);
}

int main(void) {
    test_boolmap_tagged_bool_keys_only();
    test_boolmap_f32_tagged_value_still_narrows();

    if (g_failed == 0) {
        printf("test_xrt_boolmap: %d passed, %d failed\n", g_passed, g_failed);
        return 0;
    }
    printf("test_xrt_boolmap: %d passed, %d failed\n", g_passed, g_failed);
    return 1;
}
