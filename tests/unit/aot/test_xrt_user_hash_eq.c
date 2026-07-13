#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#define XRT_DATA_ALIGN 32

static int g_passed;
static int g_failed;
static int g_hash_calls;
static int g_eq_calls;

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

typedef struct {
    int64_t value;
} TestToken;

XRT_COLD _Noreturn void xrt_throw_exc(XrValue exc) {
    (void) exc;
    fprintf(stderr, "unexpected xrt_throw_exc in test_xrt_user_hash_eq\n");
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

#define ASSERT_NULL(value, msg)                                                                    \
    do {                                                                                           \
        XrValue _actual = (value);                                                                 \
        if (_actual.tag != XR_TAG_NULL) {                                                          \
            fprintf(stderr, "FAIL: %s (got tag %d)\n", msg, _actual.tag);                          \
            g_failed++;                                                                            \
            return;                                                                                \
        }                                                                                          \
        g_passed++;                                                                                \
    } while (0)

static XrValue make_token(uint16_t type_id, int64_t value) {
    TestToken *token = (TestToken *) xrt_obj_alloc(type_id, sizeof(TestToken));
    token->value = value;
    return xrt_box_obj(token);
}

static int64_t token_hash(xrt_closure_t *closure, void *value) {
    (void) closure;
    g_hash_calls++;
    return ((const TestToken *) value)->value;
}

static uint8_t token_eq(xrt_closure_t *closure, void *a, void *b) {
    (void) closure;
    g_eq_calls++;
    return ((const TestToken *) a)->value == ((const TestToken *) b)->value;
}

static XrValue make_map_key(const char *class_name, int64_t value) {
    XrValue key = xrt_map_new(0);
    xrt_map_set_class_name(key, class_name);
    xrt_setprop_name(key, "value", XR_FROM_INT(value));
    return key;
}

static int64_t map_key_hash(xrt_closure_t *closure, void *value) {
    (void) closure;
    g_hash_calls++;
    XrValue key = xr_mkheap(value, XR_TMAP);
    return xrt_getprop_name(key, "value").i;
}

static uint8_t map_key_eq(xrt_closure_t *closure, void *a, void *b) {
    (void) closure;
    g_eq_calls++;
    XrValue lhs = xr_mkheap(a, XR_TMAP);
    XrValue rhs = xr_mkheap(b, XR_TMAP);
    return (uint8_t) (xrt_eq(xrt_getprop_name(lhs, "value"), xrt_getprop_name(rhs, "value")) != 0);
}

static void reset_call_counts(void) {
    g_hash_calls = 0;
    g_eq_calls = 0;
}

static void test_direct_helpers_gate_on_exact_native_type(void) {
    uint16_t token_type = xrt_type_register_hot(0, NULL, 0, NULL, sizeof(TestToken));
    uint16_t other_type = xrt_type_register_hot(0, NULL, 0, NULL, sizeof(TestToken));
    XrValue token_a = make_token(token_type, 7);
    XrValue token_b = make_token(token_type, 7);
    XrValue other = make_token(other_type, 7);

    reset_call_counts();
    ASSERT_EQ_INT(xrt_user_hash_value(token_a, token_type, NULL, token_hash), 7,
                  "exact token uses user hash");
    ASSERT_EQ_INT(g_hash_calls, 1, "exact hash called once");

    reset_call_counts();
    ASSERT_TRUE(xrt_user_hash_value(other, token_type, NULL, token_hash) != 7,
                "wrong exact type falls back to generic hash");
    ASSERT_EQ_INT(g_hash_calls, 0, "wrong type never calls user hash");

    reset_call_counts();
    ASSERT_TRUE(xrt_user_eq_value(token_a, token_b, token_type, NULL, token_eq),
                "two exact tokens use user equality");
    ASSERT_EQ_INT(g_eq_calls, 1, "exact equality called once");

    reset_call_counts();
    ASSERT_TRUE(!xrt_user_eq_value(token_a, other, token_type, NULL, token_eq),
                "mixed exact/wrong type does not compare equal");
    ASSERT_EQ_INT(g_eq_calls, 0, "mixed type never calls user equality");
}

static void test_direct_helpers_gate_on_map_backed_class_name(void) {
    uint16_t token_type = xrt_type_register_hot(0, NULL, 0, NULL, sizeof(TestToken));
    XrValue key_a = make_map_key("Key", 17);
    XrValue key_b = make_map_key("Key", 17);
    XrValue other = make_map_key("Other", 17);

    reset_call_counts();
    ASSERT_EQ_INT(xrt_user_hash_value(key_a, token_type, "Key", map_key_hash), 17,
                  "map-backed class name uses user hash");
    ASSERT_EQ_INT(g_hash_calls, 1, "map-backed hash called once");

    reset_call_counts();
    ASSERT_TRUE(xrt_user_eq_value(key_a, key_b, token_type, "Key", map_key_eq),
                "matching map-backed class names use user equality");
    ASSERT_EQ_INT(g_eq_calls, 1, "map-backed equality called once");

    reset_call_counts();
    ASSERT_TRUE(!xrt_user_eq_value(key_a, other, token_type, "Key", map_key_eq),
                "mixed map-backed class names do not compare equal");
    ASSERT_EQ_INT(g_eq_calls, 0, "wrong map-backed class never calls user equality");
}

static void test_map_user_hash_eq_wrong_type_falls_back_without_user_calls(void) {
    uint16_t token_type = xrt_type_register_hot(0, NULL, 0, NULL, sizeof(TestToken));
    uint16_t other_type = xrt_type_register_hot(0, NULL, 0, NULL, sizeof(TestToken));
    XrValue token = make_token(token_type, 11);
    XrValue other = make_token(other_type, 11);
    XrValue map_value = xrt_map_new(0);
    xrt_map_t *map = (xrt_map_t *) map_value.ptr;

    xrt_map_set_user_hash_eq(map, token, XR_FROM_INT(99), token_type, NULL, token_hash, token_eq);

    reset_call_counts();
    ASSERT_TRUE(xrt_map_has_user_hash_eq(map, token, token_type, NULL, token_hash, token_eq),
                "exact token lookup uses user hash/eq path");
    ASSERT_TRUE(g_hash_calls > 0, "exact map lookup calls user hash");
    ASSERT_TRUE(g_eq_calls > 0, "exact map lookup calls user equality");

    reset_call_counts();
    ASSERT_TRUE(!xrt_map_has_user_hash_eq(map, other, token_type, NULL, token_hash, token_eq),
                "wrong-type map lookup must not hit exact token");
    ASSERT_EQ_INT(g_hash_calls, 0, "wrong-type map lookup does not call user hash");
    ASSERT_EQ_INT(g_eq_calls, 0, "wrong-type map lookup does not call user equality");
    ASSERT_NULL(xrt_map_get_user_hash_eq(map, other, token_type, NULL, token_hash, token_eq),
                "wrong-type map get returns missing");

    reset_call_counts();
    xrt_map_set_user_hash_eq(map, other, XR_FROM_INT(55), token_type, NULL, token_hash, token_eq);
    ASSERT_EQ_INT(g_hash_calls, 0, "wrong-type map set does not call user hash");
    ASSERT_EQ_INT(g_eq_calls, 0, "wrong-type map set does not call user equality");
    ASSERT_EQ_INT(xrt_map_get_user_hash_eq(map, other, token_type, NULL, token_hash, token_eq).i,
                  55, "wrong-type fallback entry round-trips through generic equality");
    ASSERT_EQ_INT(g_hash_calls, 0, "wrong-type map get still does not call user hash");
    ASSERT_EQ_INT(g_eq_calls, 0, "wrong-type map get still does not call user equality");

    xrt_map_destroy(map);
}

static void test_map_user_hash_eq_map_backed_class_name(void) {
    uint16_t token_type = xrt_type_register_hot(0, NULL, 0, NULL, sizeof(TestToken));
    XrValue key = make_map_key("Key", 19);
    XrValue equal_key = make_map_key("Key", 19);
    XrValue other_class = make_map_key("Other", 19);
    XrValue map_value = xrt_map_new(0);
    xrt_map_t *map = (xrt_map_t *) map_value.ptr;

    xrt_map_set_user_hash_eq(map, key, XR_FROM_INT(88), token_type, "Key", map_key_hash,
                             map_key_eq);

    reset_call_counts();
    ASSERT_TRUE(
        xrt_map_has_user_hash_eq(map, equal_key, token_type, "Key", map_key_hash, map_key_eq),
        "matching map-backed class lookup uses user hash/eq path");
    ASSERT_TRUE(g_hash_calls > 0, "map-backed lookup calls user hash");
    ASSERT_TRUE(g_eq_calls > 0, "map-backed lookup calls user equality");
    ASSERT_EQ_INT(
        xrt_map_get_user_hash_eq(map, equal_key, token_type, "Key", map_key_hash, map_key_eq).i, 88,
        "map-backed equal key gets stored value");

    reset_call_counts();
    ASSERT_TRUE(
        !xrt_map_has_user_hash_eq(map, other_class, token_type, "Key", map_key_hash, map_key_eq),
        "wrong map-backed class does not hit specialized entry");
    ASSERT_EQ_INT(g_hash_calls, 0, "wrong map-backed class does not call user hash");
    ASSERT_EQ_INT(g_eq_calls, 0, "wrong map-backed class does not call user equality");

    xrt_map_destroy(map);
}

static void test_set_user_hash_eq_wrong_type_falls_back_without_user_calls(void) {
    uint16_t token_type = xrt_type_register_hot(0, NULL, 0, NULL, sizeof(TestToken));
    uint16_t other_type = xrt_type_register_hot(0, NULL, 0, NULL, sizeof(TestToken));
    XrValue token = make_token(token_type, 13);
    XrValue other = make_token(other_type, 13);
    XrValue set_value = xrt_set_new(0);
    xrt_set_t *set = (xrt_set_t *) set_value.ptr;

    ASSERT_TRUE(xrt_set_add_user_hash_eq(set, token, token_type, NULL, token_hash, token_eq),
                "exact token inserted");

    reset_call_counts();
    ASSERT_TRUE(xrt_set_has_user_hash_eq(set, token, token_type, NULL, token_hash, token_eq),
                "exact token set lookup uses user path");
    ASSERT_TRUE(g_hash_calls > 0, "exact set lookup calls user hash");
    ASSERT_TRUE(g_eq_calls > 0, "exact set lookup calls user equality");

    reset_call_counts();
    ASSERT_TRUE(!xrt_set_has_user_hash_eq(set, other, token_type, NULL, token_hash, token_eq),
                "wrong-type set lookup must not hit exact token");
    ASSERT_EQ_INT(g_hash_calls, 0, "wrong-type set lookup does not call user hash");
    ASSERT_EQ_INT(g_eq_calls, 0, "wrong-type set lookup does not call user equality");

    reset_call_counts();
    ASSERT_TRUE(xrt_set_add_user_hash_eq(set, other, token_type, NULL, token_hash, token_eq),
                "wrong-type set insert falls back to generic equality");
    ASSERT_EQ_INT(g_hash_calls, 0, "wrong-type set add does not call user hash");
    ASSERT_EQ_INT(g_eq_calls, 0, "wrong-type set add does not call user equality");
    ASSERT_TRUE(xrt_set_has_user_hash_eq(set, other, token_type, NULL, token_hash, token_eq),
                "wrong-type fallback set entry round-trips");
    ASSERT_EQ_INT(g_hash_calls, 0, "wrong-type set has still does not call user hash");
    ASSERT_EQ_INT(g_eq_calls, 0, "wrong-type set has still does not call user equality");

    xrt_set_destroy(set);
}

int main(void) {
    test_direct_helpers_gate_on_exact_native_type();
    test_direct_helpers_gate_on_map_backed_class_name();
    test_map_user_hash_eq_wrong_type_falls_back_without_user_calls();
    test_map_user_hash_eq_map_backed_class_name();
    test_set_user_hash_eq_wrong_type_falls_back_without_user_calls();

    if (g_failed == 0) {
        printf("test_xrt_user_hash_eq: %d passed, %d failed\n", g_passed, g_failed);
        return 0;
    }
    printf("test_xrt_user_hash_eq: %d passed, %d failed\n", g_passed, g_failed);
    return 1;
}
