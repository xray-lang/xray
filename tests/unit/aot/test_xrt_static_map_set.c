#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include "../test_win_compat.h"

#define XRT_DATA_ALIGN 32

static int g_passed;
static int g_failed;
static int g_free_count;

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
    if (ptr)
        g_free_count++;
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
    fprintf(stderr, "unexpected xrt_throw_exc in test_xrt_static_map_set\n");
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

static void test_static_map_storage_uses_prehashed_lookup(void) {
    xrt_map_t map;
    uint8_t ctrl[XR_SWISS_GROUP * 2];
    int32_t indices[XR_SWISS_GROUP];
    XrMapEntry entries[5];
    XrValue key = xr_box_str("ada");
    uint32_t hash = xrt_hash32_value(key);

    /* String keys force tagged entry storage, so the declared key element stays
     * XR_ELEM_ANY while the scalar value element is recorded. */
    XrValue value = xrt_map_static_storage_init(&map, ctrl, indices, entries, XR_SWISS_GROUP, 5,
                                                XR_ELEM_ANY, XR_ELEM_I64);
    xrt_map_set_prehashed(&map, key, XR_FROM_INT(7), hash);
    (void) xrt_map_static_storage_freeze(&map);

    ASSERT_TRUE(value.ptr == &map, "static Map value points at caller-owned storage");
    ASSERT_TRUE((map.hdr.extra & XR_OBJ_STORAGE_BUMP) != 0,
                "static Map has non-releasing lifetime header");
    ASSERT_TRUE((map.flags & XR_MAP_FLAG_STATIC_READONLY) != 0, "static Map freezes as readonly");
    ASSERT_EQ_INT(xrt_map_get_prehashed(&map, key, hash).i, 7,
                  "static Map consumes producer prehash");
    xrt_coll_retain(value);
    xrt_coll_release(value);
}

static void test_static_set_storage_uses_prehashed_lookup(void) {
    xrt_set_t set;
    uint8_t ctrl[XR_SWISS_GROUP * 2];
    int32_t indices[XR_SWISS_GROUP];
    XrSetEntry entries[5];
    XrValue key = xr_box_str("lin");
    uint32_t hash = xrt_hash32_value(key);

    XrValue value = xrt_set_static_storage_init(&set, ctrl, indices, entries, XR_SWISS_GROUP, 5);
    (void) xrt_set_add_prehashed(&set, key, hash);
    (void) xrt_set_static_storage_freeze(&set);

    ASSERT_TRUE(value.ptr == &set, "static Set value points at caller-owned storage");
    ASSERT_TRUE((set.hdr.extra & XR_OBJ_STORAGE_BUMP) != 0,
                "static Set has non-releasing lifetime header");
    ASSERT_TRUE((set.flags & XR_SET_FLAG_STATIC_READONLY) != 0, "static Set freezes as readonly");
    ASSERT_TRUE(xrt_set_has_prehashed(&set, key, hash), "static Set consumes producer prehash");
    xrt_coll_retain(value);
    xrt_coll_release(value);
}

static void test_tagged_set_clear_releases_owned_values(void) {
    XrValue set_value = xrt_set_new(0);
    xrt_set_t *set = (xrt_set_t *) set_value.ptr;
    XrValue item = xr_box_str("owned-by-set");
    int before_clear;

    ASSERT_TRUE(xrt_set_add(set, item) == 1, "tagged Set accepts owned string value");
    before_clear = g_free_count;
    xrt_set_clear(set);

    ASSERT_TRUE(g_free_count > before_clear, "tagged Set.clear releases owned values");
    ASSERT_EQ_INT(set->count, 0, "tagged Set.clear resets count");
    ASSERT_EQ_INT(set->nentries, 0, "tagged Set.clear resets entry cursor");
    xrt_set_destroy(set);
}

int main(void) {
    test_static_map_storage_uses_prehashed_lookup();
    test_static_set_storage_uses_prehashed_lookup();
    test_tagged_set_clear_releases_owned_values();
    printf("test_xrt_static_map_set: %d passed, %d failed\n", g_passed, g_failed);
    return g_failed == 0 ? 0 : 1;
}
