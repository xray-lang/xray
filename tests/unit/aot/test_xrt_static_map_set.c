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
    ASSERT_TRUE((map.hdr.extra & XR_OBJ_IMMORTAL) != 0,
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
    ASSERT_TRUE((set.hdr.extra & XR_OBJ_IMMORTAL) != 0,
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

static void test_tagged_map_nan_key_has_canonical_identity(void) {
    XrValue map_value = xrt_map_new(0);
    xrt_map_t *map = (xrt_map_t *) map_value.ptr;
    XrValue first_nan = XR_FROM_FLOAT(NAN);
    XrValue second_nan = XR_FROM_FLOAT(nan("xray"));

    xrt_map_set(map, first_nan, XR_FROM_INT(7));
    xrt_map_set(map, second_nan, XR_FROM_INT(9));

    ASSERT_EQ_INT(xrt_map_len(map), 1, "all NaN payloads identify one Map key");
    ASSERT_TRUE(xrt_map_has(map, first_nan), "a NaN key is reflexively findable");
    ASSERT_EQ_INT(xrt_map_get(map, first_nan).i, 9, "a later NaN write overwrites the key");
    xrt_map_destroy(map);
}

static void test_tagged_map_preserves_declared_scalar_key_lane(void) {
    XrValue map_value = xrt_map_new_declared(0, XR_ELEM_F64, XR_ELEM_ANY);
    xrt_map_t *map = (xrt_map_t *) map_value.ptr;

    xrt_map_set(map, XR_FROM_FLOAT(1.0), xr_box_str("one"));
    XrValue keys = xrt_map_keys(map);
    xrt_array_t *key_array = (xrt_array_t *) keys.ptr;

    ASSERT_EQ_INT(key_array->elem_type, XR_ELEM_F64,
                  "Map<float, tagged>.keys keeps packed float lanes");
    ASSERT_EQ_INT(key_array->length, 1, "keys returns the declared scalar key");
    ASSERT_TRUE(((double *) key_array->data)[0] == 1.0,
                "the packed float key is not misread as tagged storage");
    xrt_release(keys);
    xrt_map_destroy(map);
}

static void test_collection_materialization_promotes_reference_elements(void) {
    XrValue map_value = xrt_map_new(0);
    xrt_map_t *map = (xrt_map_t *) map_value.ptr;
    XrValue map_key = xr_box_str("owned-key");
    XrValue map_value_item = xr_box_str("owned-value");
    xrt_map_set(map, map_key, map_value_item);

    XrValue keys = xrt_map_keys(map);
    XrValue values = xrt_map_values(map);
    ASSERT_EQ_INT(
        atomic_load_explicit(&xrt_arc_value_header(map_key)->refcount, memory_order_relaxed), 1,
        "Map.keys promotes a reference element");
    ASSERT_EQ_INT(
        atomic_load_explicit(&xrt_arc_value_header(map_value_item)->refcount, memory_order_relaxed),
        1, "Map.values promotes a reference element");
    xrt_release(keys);
    xrt_release(values);
    ASSERT_TRUE(strcmp(xr_str_data(map_key), "owned-key") == 0,
                "releasing Map.keys preserves the map's key owner");
    ASSERT_TRUE(strcmp(xr_str_data(map_value_item), "owned-value") == 0,
                "releasing Map.values preserves the map's value owner");
    xrt_map_destroy(map);

    XrValue set_value = xrt_set_new(0);
    xrt_set_t *set = (xrt_set_t *) set_value.ptr;
    XrValue set_item = xr_box_str("owned-set-value");
    ASSERT_TRUE(xrt_set_add(set, set_item) == 1, "Set accepts the reference element");
    XrValue set_values = xrt_set_values(set);
    ASSERT_EQ_INT(
        atomic_load_explicit(&xrt_arc_value_header(set_item)->refcount, memory_order_relaxed), 1,
        "Set.values promotes a reference element");
    xrt_release(set_values);
    ASSERT_TRUE(strcmp(xr_str_data(set_item), "owned-set-value") == 0,
                "releasing Set.values preserves the set's element owner");
    xrt_set_destroy(set);
}

static void test_shared_promotion_preserves_all_existing_owners(void) {
    XrValue value = xrt_str_from_cstr("abc");
    XrObjHeader *header = xrt_arc_value_header(value);

    xrt_retain(value);
    xrt_retain(value);
    ASSERT_EQ_INT(atomic_load_explicit(&header->refcount, memory_order_relaxed), 2,
                  "local RC encodes three owners as two");

    (void) xrt_value_set_storage(value, XR_OBJ_STORAGE_SHARED);
    ASSERT_EQ_INT(atomic_load_explicit(&header->refcount, memory_order_relaxed), -3,
                  "shared promotion preserves all three owners");

    xrt_release(value);
    ASSERT_EQ_INT(xr_str_len(value), 3, "first shared release keeps the string alive");
    xrt_release(value);
    ASSERT_TRUE(strcmp(xr_str_data(value), "abc") == 0,
                "second shared release keeps the final owner valid");

    int before_final_release = g_free_count;
    xrt_release(value);
    ASSERT_TRUE(g_free_count > before_final_release,
                "final shared release destroys the promoted string");
}

int main(void) {
    test_static_map_storage_uses_prehashed_lookup();
    test_static_set_storage_uses_prehashed_lookup();
    test_tagged_set_clear_releases_owned_values();
    test_tagged_map_nan_key_has_canonical_identity();
    test_tagged_map_preserves_declared_scalar_key_lane();
    test_collection_materialization_promotes_reference_elements();
    test_shared_promotion_preserves_all_existing_owners();
    printf("test_xrt_static_map_set: %d passed, %d failed\n", g_passed, g_failed);
    return g_failed == 0 ? 0 : 1;
}
