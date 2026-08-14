/* The VM and the AOT runtime must answer the same public type id with the same
 * user-visible name. They used to keep private tables and drifted: a bound
 * method printed "function" under the VM and "bound_method" under AOT, and
 * CountdownLatch/Semaphore/EventCount/Buffer/JSON.Value degraded to "unknown"
 * on one side or the other in `as T` mismatch messages. Both now expand
 * xr_type_names.def; this test is what keeps a second table from reappearing. */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
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
#include "../../../src/aot/xrt_exception.h"
#if defined(__clang__)
#pragma clang diagnostic pop
#endif

#define ASSERT_STR_EQ(actual, expected, msg)                                                       \
    do {                                                                                           \
        const char *_a = (actual);                                                                 \
        const char *_e = (expected);                                                               \
        if (!_a || !_e || strcmp(_a, _e) != 0) {                                                   \
            fprintf(stderr, "FAIL: %s (got \"%s\", expected \"%s\")\n", msg, _a ? _a : "(null)",   \
                    _e ? _e : "(null)");                                                           \
            g_failed++;                                                                            \
        } else {                                                                                   \
            g_passed++;                                                                            \
        }                                                                                          \
    } while (0)

/* Every row of the registry, VM answer against AOT answer against the spelling
 * the row itself declares. */
static void test_every_id_agrees_across_backends(void) {
#define XR_TYPE_NAME(suffix, id, display)                                                          \
    ASSERT_STR_EQ(xr_typeid_name(XR_TID_##suffix), display, "VM name for " #suffix);               \
    ASSERT_STR_EQ(xrt_type_name(XR_TID_##suffix), display, "AOT name for " #suffix);               \
    ASSERT_STR_EQ(xrt_type_name(XR_TID_##suffix), xr_typeid_name(XR_TID_##suffix),                 \
                  "VM/AOT agreement for " #suffix);
#include "../../../src/shared/xr_type_names.def"
#undef XR_TYPE_NAME
}

/* The ids that actually drifted, pinned by literal so a future edit to the
 * registry has to face the string a user sees. */
static void test_previously_drifted_ids(void) {
    ASSERT_STR_EQ(xr_typeid_name(24), "PanicInfo", "tid 24 VM");
    ASSERT_STR_EQ(xrt_type_name(24), "PanicInfo", "tid 24 AOT");
    /* A bound method is a callable and reports what every other callable
     * reports. "bound_method" was an AOT-only spelling. */
    ASSERT_STR_EQ(xr_typeid_name(27), "function", "tid 27 VM");
    ASSERT_STR_EQ(xrt_type_name(27), "function", "tid 27 AOT");
    ASSERT_STR_EQ(xr_typeid_name(38), "CountdownLatch", "tid 38 VM");
    ASSERT_STR_EQ(xrt_type_name(38), "CountdownLatch", "tid 38 AOT");
    ASSERT_STR_EQ(xr_typeid_name(39), "Semaphore", "tid 39 VM");
    ASSERT_STR_EQ(xrt_type_name(39), "Semaphore", "tid 39 AOT");
    ASSERT_STR_EQ(xr_typeid_name(40), "EventCount", "tid 40 VM");
    ASSERT_STR_EQ(xrt_type_name(40), "EventCount", "tid 40 AOT");
    ASSERT_STR_EQ(xr_typeid_name(42), "Buffer", "tid 42 VM");
    ASSERT_STR_EQ(xrt_type_name(42), "Buffer", "tid 42 AOT");
    ASSERT_STR_EQ(xr_typeid_name(43), "rune", "tid 43 VM");
    ASSERT_STR_EQ(xrt_type_name(43), "rune", "tid 43 AOT");
    ASSERT_STR_EQ(xr_typeid_name(44), "JSON.Value", "tid 44 VM");
    ASSERT_STR_EQ(xrt_type_name(44), "JSON.Value", "tid 44 AOT");
}

/* No id inside the registry may fall through to the unknown placeholder, and
 * both sides must still answer identically outside it. */
static void test_registry_is_total(void) {
    for (int tid = 0; tid < XR_TID_COUNT; tid++) {
        const char *vm = xr_typeid_name((XrTypeId) tid);
        const char *aot = xrt_type_name(tid);
        if (strcmp(vm, aot) != 0) {
            fprintf(stderr, "FAIL: tid %d disagrees (VM \"%s\", AOT \"%s\")\n", tid, vm, aot);
            g_failed++;
            continue;
        }
        if (strcmp(vm, "unknown") == 0) {
            fprintf(stderr, "FAIL: tid %d has no registered name\n", tid);
            g_failed++;
            continue;
        }
        g_passed++;
    }
    ASSERT_STR_EQ(xr_typeid_name((XrTypeId) XR_TID_COUNT), "unknown", "out-of-range VM");
    ASSERT_STR_EQ(xrt_type_name(XR_TID_COUNT), "unknown", "out-of-range AOT");
    ASSERT_STR_EQ(xrt_type_name(-1), "unknown", "negative id AOT");
    ASSERT_STR_EQ(xr_typeid_name((XrTypeId) -1), "unknown", "negative id VM");
}

int main(void) {
    test_every_id_agrees_across_backends();
    test_previously_drifted_ids();
    test_registry_is_total();
    if (g_failed != 0) {
        fprintf(stderr, "test_type_names_parity: %d passed, %d failed\n", g_passed, g_failed);
        return 1;
    }
    printf("test_type_names_parity: %d passed\n", g_passed);
    return 0;
}
