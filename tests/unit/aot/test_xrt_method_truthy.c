#include <math.h>
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

#define XRT_ENABLE_SYS_THREAD 1
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

#define ASSERT_INT(value, expected, msg)                                                           \
    do {                                                                                           \
        XrValue _actual = (value);                                                                 \
        if (_actual.tag != XR_TAG_I64 || _actual.i != (expected)) {                                \
            fprintf(stderr, "FAIL: %s (got tag %d value %lld, expected %lld)\n", msg, _actual.tag, \
                    (long long) _actual.i, (long long) (expected));                                \
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

#define ASSERT_TRUE_MSG(cond, msg)                                                                 \
    do {                                                                                           \
        if (!(cond)) {                                                                             \
            fprintf(stderr, "FAIL: %s\n", msg);                                                    \
            g_failed++;                                                                            \
            return;                                                                                \
        }                                                                                          \
        g_passed++;                                                                                \
    } while (0)

#define ASSERT_CSTR(actual, expected, msg)                                                         \
    do {                                                                                           \
        const char *_actual = (actual);                                                            \
        const char *_expected = (expected);                                                        \
        if (!_actual || strcmp(_actual, _expected) != 0) {                                         \
            fprintf(stderr, "FAIL: %s (got %s, expected %s)\n", msg, _actual ? _actual : "<null>", \
                    _expected);                                                                    \
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
    XrAotEnumBox enum_box = {0};

    ASSERT_WEAK_ACCEPTS(XR_NULL_VAL, false, "null is not a weak key object");
    ASSERT_WEAK_ACCEPTS(XR_FROM_INT(1), false, "int is not a weak key object");
    ASSERT_WEAK_ACCEPTS(XR_TRUE_VAL, false, "bool is not a weak key object");
    ASSERT_WEAK_ACCEPTS(xrt_range_from_i64(1, 4, false), true, "Range is an object-like weak key");
    ASSERT_WEAK_ACCEPTS(xr_mkptr(&enum_box, XR_TAG_ENUM), true,
                        "Enum box is an object-like weak key");
    ASSERT_WEAK_ACCEPTS(xr_mkptr(&dummy, XR_TAG_ITERATOR), true,
                        "Iterator is an object-like weak key");
    ASSERT_WEAK_ACCEPTS(xr_aggregate_ref(&dummy, 1), true,
                        "native struct reference is an object-like weak key");
}

static void test_xrt_type_metadata_uses_hot_name_and_derive_tables(void) {
    uint16_t tid = xrt_type_register_hot(0, NULL, 0, NULL, 16);
    const XrtTypeInfo *hot = xrt_type_info(tid);
    const XrtTypeNameInfo *name = xrt_type_name_info(tid);
    const XrtTypeDeriveInfo *derive = xrt_type_derive_info(tid);
    ASSERT_TRUE_MSG(hot != NULL, "type hot row is registered");
    ASSERT_TRUE_MSG(name != NULL, "type name row is registered");
    ASSERT_TRUE_MSG(derive != NULL, "type derive row is registered");
    ASSERT_TRUE_MSG(hot->type_id == tid && hot->instance_size == 16,
                    "hot row carries identity and instance size");
    ASSERT_TRUE_MSG(xrt_type_display_name(tid) == NULL,
                    "hot registration starts without name metadata");
    xrt_type_set_name(tid, "Box$i64", NULL);
    ASSERT_CSTR(name->name, "Box$i64", "name row carries internal name");
    ASSERT_CSTR(xrt_type_display_name(tid), "Box$i64", "display falls back to internal name");
    ASSERT_TRUE_MSG(derive->derive_flags == 0 && derive->inspect_field_count == 0,
                    "derive row starts empty");

    static const char *args[] = {"int"};
    xrt_type_set_generic_origin(tid, tid);
    xrt_type_set_generic_name(tid, "Box<int>", args, 1);
    ASSERT_TRUE_MSG(hot->generic_origin == tid, "generic origin stays in hot row");
    ASSERT_CSTR(xrt_type_display_name(tid), "Box<int>", "display name comes from name row");
    ASSERT_TRUE_MSG(name->mono_type_argc == 1 && name->mono_type_arg_names == args,
                    "generic type args stay in name row");

    static const XrtInspectField fields[] = {{"value", 0, XR_NATIVE_I64}};
    xrt_type_set_derive(tid, XR_DERIVE_JSON | XR_DERIVE_INSPECT, fields, 1);
    ASSERT_TRUE_MSG((derive->derive_flags & XR_DERIVE_JSON) != 0, "derive row carries Json flag");
    ASSERT_TRUE_MSG((derive->derive_flags & XR_DERIVE_INSPECT) != 0,
                    "derive row carries Inspect flag");
    ASSERT_TRUE_MSG(derive->inspect_fields == fields && derive->inspect_field_count == 1,
                    "derive row carries inspect sidecar");
    ASSERT_TRUE_MSG(xrt_type_internal_name_eq(tid, "Box$i64"),
                    "internal-name lookup reads name row");
}

static void test_xrt_thread_handle_methods(void) {
    xrt_thread_object_t thread = {0};
    atomic_store_explicit(&thread.state, XRT_THREAD_JOINED, memory_order_release);
    atomic_store_explicit(&thread.finished, true, memory_order_release);
    thread.retval = XR_FROM_INT(42);

    XrValue handle = xrt_thread_box(&thread);
    ASSERT_WEAK_ACCEPTS(handle, true, "Thread handle is an object-like weak key");
    ASSERT_BOOL(xrt_thread_done_value(handle), true, "Thread.done reads finished flag");
    ASSERT_INT(xrt_method_0(handle, XRT_SYM_JOIN), 42, "Thread.join returns cached retval");
    ASSERT_NULL(xrt_method_0(handle, XRT_SYM_DETACH), "Thread.detach is a null-returning method");
}

static _Atomic uint64_t g_threadlocal_test_id;
static atomic_int g_threadlocal_test_stage;

static void *threadlocal_live_test_entry(void *arg) {
    (void) arg;
    xrt_threadlocal_enter_current();
    atomic_store_explicit(&g_threadlocal_test_id, (uint64_t) XR_TO_INT(xrt_sys_thread_local_id()),
                          memory_order_release);
    atomic_store_explicit(&g_threadlocal_test_stage, 1, memory_order_release);
    while (atomic_load_explicit(&g_threadlocal_test_stage, memory_order_acquire) == 1)
        xr_thread_yield();
    xrt_threadlocal_leave_current();
    atomic_store_explicit(&g_threadlocal_test_stage, 3, memory_order_release);
    return NULL;
}

static void test_xrt_threadlocal_live_registry(void) {
    atomic_store_explicit(&g_threadlocal_test_id, 0, memory_order_release);
    atomic_store_explicit(&g_threadlocal_test_stage, 0, memory_order_release);

    xr_thread_t thread;
    ASSERT_TRUE_MSG(xr_thread_create(&thread, threadlocal_live_test_entry, NULL),
                    "threadlocal live registry test thread starts");
    while (atomic_load_explicit(&g_threadlocal_test_stage, memory_order_acquire) == 0)
        xr_thread_yield();

    uint64_t id = atomic_load_explicit(&g_threadlocal_test_id, memory_order_acquire);
    uint64_t current_id = (uint64_t) XR_TO_INT(xrt_sys_thread_local_id());
    ASSERT_TRUE_MSG(id != 0 && id != current_id,
                    "threadlocal live registry captures child thread token");
    ASSERT_BOOL(xrt_sys_thread_local_alive(XR_FROM_INT((int64_t) id)), true,
                "running sys.Thread token is alive");

    atomic_store_explicit(&g_threadlocal_test_stage, 2, memory_order_release);
    xr_thread_join(thread, NULL);
    ASSERT_BOOL(xrt_sys_thread_local_alive(XR_FROM_INT((int64_t) id)), false,
                "exited sys.Thread token is not alive");
    ASSERT_BOOL(xrt_sys_thread_local_alive(XR_FROM_INT((int64_t) current_id)), true,
                "current thread token is always alive");
}

int main(void) {
    test_xrt_to_bool_reuses_truthy_core_for_scalars_and_strings();
    test_xrt_to_bool_reuses_truthy_core_for_sized_containers();
    test_xrt_weak_predicate_accepts_aot_object_tags();
    test_xrt_type_metadata_uses_hot_name_and_derive_tables();
    test_xrt_thread_handle_methods();
    test_xrt_threadlocal_live_registry();

    if (g_failed == 0) {
        printf("test_xrt_method_truthy: %d passed, %d failed\n", g_passed, g_failed);
        return 0;
    }
    printf("test_xrt_method_truthy: %d passed, %d failed\n", g_passed, g_failed);
    return 1;
}
