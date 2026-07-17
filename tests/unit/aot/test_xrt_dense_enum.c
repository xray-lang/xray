#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include "../test_win_compat.h"

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
    return xr_test_alloc_aligned(size, XRT_DATA_ALIGN);
}

static void test_free_aligned(void *ptr) {
    xr_test_free_aligned(ptr);
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
    fprintf(stderr, "unexpected xrt_throw_exc in test_xrt_dense_enum\n");
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

typedef struct TestEnumBox {
    uint64_t gc_words[2];
    void *klass;
    const char *enum_name;
    const char *member_name;
    uint32_t member_index;
    uint32_t payload_count;
    uint32_t layout_id;
} TestEnumBox;

static XrValue test_enum_value(TestEnumBox *box) {
    XrValue value = {0};
    value.tag = XR_TAG_ENUM;
    value.ext = (uint16_t) box->member_index;
    value.ptr = box;
    return value;
}

static void init_color_boxes(TestEnumBox boxes[5]) {
    static const char *names[5] = {"Red", "Green", "Blue", "Yellow", "Purple"};
    for (uint32_t i = 0; i < 5; i++)
        boxes[i] = (TestEnumBox) {{0, 0}, NULL, "Color", names[i], i, 0, 17};
}

static void test_dense_enum_map_hit_and_mutation_fallback(void) {
    TestEnumBox stored[5];
    TestEnumBox queries[5];
    init_color_boxes(stored);
    init_color_boxes(queries);
    XrValue map_value = xrt_map_new(5);
    xrt_map_t *map = (xrt_map_t *) map_value.ptr;
    for (uint32_t i = 0; i < 5; i++)
        xrt_map_set(map, test_enum_value(&stored[i]), XR_FROM_INT(10 + i));

    ASSERT_EQ_INT(xrt_map_find_dense_enum_slot(map, test_enum_value(&queries[2])), 2,
                  "semantic enum equality confirms the ordinal map slot");
    ASSERT_EQ_INT(xrt_map_get_dense_enum(map, test_enum_value(&queries[2])).i, 12,
                  "dense enum map get returns the ordinal value");
    ASSERT_TRUE(xrt_map_has_dense_enum(map, test_enum_value(&queries[4])),
                "dense enum map has returns true");
    ASSERT_TRUE(!xrt_map_has_dense_enum(map, XR_FROM_INT(2)),
                "non-enum map query falls back without aliasing an ordinal");

    TestEnumBox wrong_layout = {{0, 0}, NULL, "Shade", "Blue", 2, 0, 99};
    ASSERT_TRUE(!xrt_map_has_dense_enum(map, test_enum_value(&wrong_layout)),
                "same ordinal from another enum does not alias the dense slot");

    ASSERT_TRUE(xrt_map_delete(map, test_enum_value(&stored[2])),
                "generic delete removes the dense enum key");
    ASSERT_TRUE(!xrt_map_has_dense_enum(map, test_enum_value(&queries[2])),
                "deleted ordinal falls back to a miss");
    xrt_map_set(map, test_enum_value(&stored[2]), XR_FROM_INT(42));
    ASSERT_EQ_INT(xrt_map_find_dense_enum_slot(map, test_enum_value(&stored[2])), -1,
                  "reinserted key no longer occupies its original ordinal slot");
    ASSERT_EQ_INT(xrt_map_get_dense_enum(map, test_enum_value(&stored[2])).i, 42,
                  "reinserted key is found through canonical fallback");

    xrt_map_destroy(map);
}

static void test_dense_enum_set_hit_and_mutation_fallback(void) {
    TestEnumBox stored[5];
    TestEnumBox queries[5];
    init_color_boxes(stored);
    init_color_boxes(queries);
    XrValue set_value = xrt_set_new(5);
    xrt_set_t *set = (xrt_set_t *) set_value.ptr;
    for (uint32_t i = 0; i < 5; i++)
        xrt_set_add(set, test_enum_value(&stored[i]));

    ASSERT_EQ_INT(xrt_set_find_dense_enum_slot(set, test_enum_value(&queries[3])), 3,
                  "semantic enum equality confirms the ordinal set slot");
    ASSERT_TRUE(xrt_set_has_dense_enum(set, test_enum_value(&queries[3])),
                "dense enum set has returns true");
    ASSERT_TRUE(xrt_set_delete(set, test_enum_value(&stored[3])),
                "generic delete removes the dense enum set value");
    ASSERT_TRUE(!xrt_set_has_dense_enum(set, test_enum_value(&queries[3])),
                "deleted set ordinal falls back to a miss");
    ASSERT_TRUE(xrt_set_add(set, test_enum_value(&stored[3])),
                "deleted enum set value can be reinserted");
    ASSERT_EQ_INT(xrt_set_find_dense_enum_slot(set, test_enum_value(&stored[3])), -1,
                  "reinserted set value no longer occupies its ordinal slot");
    ASSERT_TRUE(xrt_set_has_dense_enum(set, test_enum_value(&stored[3])),
                "reinserted set value is found through canonical fallback");

    xrt_set_destroy(set);
}

int main(void) {
    test_dense_enum_map_hit_and_mutation_fallback();
    test_dense_enum_set_hit_and_mutation_fallback();

    printf("test_xrt_dense_enum: %d passed, %d failed\n", g_passed, g_failed);
    return g_failed == 0 ? 0 : 1;
}
