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
static int g_test_pending_error_active;
#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wundefined-internal"
#pragma clang diagnostic ignored "-Wunused-function"
#endif
static inline int xrt_has_pending_error(void) {
    return g_test_pending_error_active;
}
#include "../../../src/aot/xrt_method.h"
#include "../../../src/base/xnumber_parse_error.h"
#if defined(__clang__)
#pragma clang diagnostic pop
#endif

XR_THREAD_LOCAL XrValue xrt_pending_error = {.tag = XR_TAG_NULL};

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

static void test_xrt_exact_number_parse_failure_channel(void) {
    const XrNumberParseErrorRegistryRow *row =
        xr_number_parse_error_registry_row(XR_GLOBAL_VAR_NUMBER_PARSE_ERROR);
    ASSERT_TRUE_MSG(row && row->global_index == XR_GLOBAL_VAR_NUMBER_PARSE_ERROR &&
                        row->enum_layout_id == XR_NUMBER_PARSE_ERROR_LAYOUT_ID,
                    "AOT parse ABI matches the compiler NumberParseError registry row");
    g_test_pending_error_active = 0;
    xrt_pending_error = XR_NULL_VAL;

    XrValue input = test_string_with_bytes("bad", 3);
    ASSERT_NULL(xrt_i64_parse(input),
                "i64.parse syntax failure returns through pending_error");
    xrt_release(input);
    XrAotEnumAggregate error = xrt_value_to_enum_aggregate(xrt_pending_error);
    ASSERT_CSTR(error.enum_name, "NumberParseError", "i64.parse publishes typed error owner");
    ASSERT_TRUE_MSG(error.layout_id == XR_NUMBER_PARSE_ERROR_LAYOUT_ID,
                    "i64.parse publishes the frozen NumberParseError type identity");
    ASSERT_CSTR(error.member_name, "InvalidSyntax", "i64 syntax failure member");
    ASSERT_TRUE_MSG(error.tag == 0, "InvalidSyntax has stable member index zero");
    xrt_release(xrt_pending_error);
    xrt_pending_error = XR_NULL_VAL;

    input = test_string_with_bytes("9223372036854775808", 19);
    ASSERT_NULL(xrt_i64_parse(input),
                "i64.parse overflow returns through pending_error");
    xrt_release(input);
    error = xrt_value_to_enum_aggregate(xrt_pending_error);
    ASSERT_CSTR(error.member_name, "OutOfRange", "i64 overflow failure member");
    ASSERT_TRUE_MSG(error.tag == 1, "OutOfRange has stable member index one");
    xrt_release(xrt_pending_error);
    xrt_pending_error = XR_NULL_VAL;

    input = test_string_with_bytes("1e+", 3);
    ASSERT_NULL(xrt_f64_parse(input),
                "f64.parse syntax failure returns through pending_error");
    xrt_release(input);
    error = xrt_value_to_enum_aggregate(xrt_pending_error);
    ASSERT_CSTR(error.member_name, "InvalidSyntax", "f64 syntax failure member");
    xrt_release(xrt_pending_error);
    xrt_pending_error = XR_NULL_VAL;

    input = test_string_with_bytes("1e400", 5);
    XrValue huge = xrt_f64_parse(input);
    xrt_release(input);
    ASSERT_TRUE_MSG(XR_IS_FLOAT(huge) && isinf(XR_TO_FLOAT(huge)) && XR_TO_FLOAT(huge) > 0.0,
                    "valid huge f64 input produces positive infinity");
    ASSERT_TRUE_MSG(XR_IS_NULL(xrt_pending_error),
                    "successful f64.parse does not publish pending_error");

    ASSERT_TRUE_MSG(!xrt_set_builtin_enum_error_by_id(
                        XR_GLOBAL_VAR_NUMBER_PARSE_ERROR + 1, 0),
                    "typed enum publisher rejects an unknown builtin id");
    ASSERT_TRUE_MSG(XR_IS_NULL(xrt_pending_error),
                    "unknown builtin id cannot write pending_error");
    ASSERT_TRUE_MSG(!xrt_set_builtin_enum_error_by_id(
                        XR_GLOBAL_VAR_NUMBER_PARSE_ERROR, 2),
                    "typed enum publisher rejects an unknown member index");
    ASSERT_TRUE_MSG(!xrt_set_builtin_enum_error_by_id(
                        XR_GLOBAL_VAR_NUMBER_PARSE_ERROR,
                        xr_number_parse_failure_member_index(XR_NUMBER_PARSE_OK)),
                    "typed enum publisher rejects the successful parser state");
    ASSERT_TRUE_MSG(!xrt_set_builtin_enum_error_by_id(
                        XR_GLOBAL_VAR_NUMBER_PARSE_ERROR,
                        xr_number_parse_failure_member_index((XrNumberParseFailure) 99)),
                    "typed enum publisher rejects a hostile parser state");
    ASSERT_TRUE_MSG(XR_IS_NULL(xrt_pending_error),
                    "unknown member index cannot write pending_error");

    input = test_string_with_bytes("bad", 3);
    ASSERT_NULL(xrt_i64_try_parse(input),
                "i64.tryParse returns null on syntax failure without pending_error");
    xrt_release(input);
    ASSERT_TRUE_MSG(XR_IS_NULL(xrt_pending_error),
                    "i64.tryParse syntax failure does not publish pending_error");
    input = test_string_with_bytes("9223372036854775808", 19);
    ASSERT_NULL(xrt_i64_try_parse(input),
                "i64.tryParse returns null on overflow without pending_error");
    xrt_release(input);
    ASSERT_TRUE_MSG(XR_IS_NULL(xrt_pending_error),
                    "i64.tryParse overflow does not publish pending_error");
    input = test_string_with_bytes("1e+", 3);
    ASSERT_NULL(xrt_f64_try_parse(input),
                "f64.tryParse returns null on syntax failure without pending_error");
    xrt_release(input);
    ASSERT_TRUE_MSG(XR_IS_NULL(xrt_pending_error),
                    "f64.tryParse syntax failure does not publish pending_error");

    XrValue sentinel = XR_FROM_INT(77);
    xrt_pending_error = sentinel;
    g_test_pending_error_active = 1;
    input = test_string_with_bytes("bad", 3);
    ASSERT_NULL(xrt_i64_try_parse(input),
                "i64.tryParse returns null on syntax failure");
    xrt_release(input);
    ASSERT_TRUE_MSG(XR_IS_INT(xrt_pending_error) && XR_TO_INT(xrt_pending_error) == 77,
                    "i64.tryParse never writes pending_error");
    input = test_string_with_bytes("1e+", 3);
    ASSERT_NULL(xrt_f64_try_parse(input),
                "f64.tryParse returns null while another error is pending");
    xrt_release(input);
    ASSERT_TRUE_MSG(XR_IS_INT(xrt_pending_error) && XR_TO_INT(xrt_pending_error) == 77,
                    "f64.tryParse never overwrites pending_error");
    ASSERT_TRUE_MSG(!xrt_set_builtin_enum_error_by_id(
                        XR_GLOBAL_VAR_NUMBER_PARSE_ERROR, 0),
                    "typed enum publisher refuses to overwrite pending_error");
    ASSERT_TRUE_MSG(XR_IS_INT(xrt_pending_error) && XR_TO_INT(xrt_pending_error) == 77,
                    "refused typed enum publication preserves pending_error");
    g_test_pending_error_active = 0;
    xrt_pending_error = XR_NULL_VAL;
}

static void test_xrt_to_bool_reuses_truthy_core_for_scalars_and_strings(void) {
    ASSERT_BOOL(xrt_to_bool(XR_NULL_VAL), false, "null is falsy");
    ASSERT_BOOL(xrt_to_bool(XR_FALSE_VAL), false, "false is falsy");
    ASSERT_BOOL(xrt_to_bool(XR_TRUE_VAL), true, "true is truthy");
    ASSERT_BOOL(xrt_to_bool(XR_FROM_INT(0)), false, "zero i64 is falsy");
    ASSERT_BOOL(xrt_to_bool(XR_FROM_INT(-1)), true, "nonzero i64 is truthy");
    ASSERT_BOOL(xrt_to_bool(XR_FROM_FLOAT(0.0)), false, "zero f64 is falsy");
    ASSERT_BOOL(xrt_to_bool(XR_FROM_FLOAT(-0.25)), true, "nonzero f64 is truthy");

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

static void test_xrt_type_identity_uses_stable_owner_adapter(void) {
    ASSERT_TRUE_MSG(xrt_typeof_id(XR_NULL_VAL) == XR_TID_NULL, "null type id");
    ASSERT_TRUE_MSG(xrt_typeof_id(XR_TRUE_VAL) == XR_TID_BOOL, "bool type id");
    ASSERT_TRUE_MSG(xrt_typeof_id(XR_FROM_INT(7)) == XR_TID_I64, "i64 type id");
    ASSERT_TRUE_MSG(xrt_typeof_id(XR_FROM_FLOAT(1.5)) == XR_TID_F64, "f64 type id");
    ASSERT_TRUE_MSG(xrt_typeof_id(XR_FROM_RUNE('X')) == XR_TID_RUNE, "rune type id");

    XrValue string = xrt_str_alloc(0);
    XrValue array = xrt_array_new(0);
    XrValue map = xrt_map_new(0);
    XrValue set = xrt_set_new(0);
    ASSERT_TRUE_MSG(xrt_typeof_id(string) == XR_TID_STRING, "string type id");
    ASSERT_TRUE_MSG(xrt_typeof_id(array) == XR_TID_ARRAY, "array type id");
    ASSERT_TRUE_MSG(xrt_typeof_id(map) == XR_TID_MAP, "map type id");
    ASSERT_TRUE_MSG(xrt_typeof_id(set) == XR_TID_SET, "set type id");
    xrt_release(string);
    xrt_release(array);
    xrt_release(map);
    xrt_release(set);
}

static void test_map_entries_dispatch_returns_tuple_array(void) {
    XrValue map = xrt_map_new(0);
    xrt_map_set((xrt_map_t *) map.ptr, xr_box_str("name"), XR_FROM_INT(7));

    XrValue entries = xrt_method_0(map, XRT_SYM_ENTRIES);
    ASSERT_TRUE_MSG(XR_IS_ARRAY(entries), "Map.entries returns an array");
    xrt_array_t *array = (xrt_array_t *) entries.ptr;
    ASSERT_TRUE_MSG(array->length == 1, "Map.entries returns every entry");
    XrValue pair = xr_typed_get(array->data, 0, array->elem_type);
    ASSERT_TRUE_MSG(pair.tag == XR_TAG_TUPLE, "Map.entries elements are tuples");
    ASSERT_CSTR(xr_str_data(xrt_tuple_get(pair, 0)), "name", "Map.entries preserves the key");
    ASSERT_INT(xrt_tuple_get(pair, 1), 7, "Map.entries preserves the value");

    xrt_release(entries);
    xrt_release(map);
}

static void test_xrt_type_metadata_uses_hot_name_and_derive_tables(void) {
    uint16_t tid = xrt_type_register_hot(0, NULL, 0, NULL, NULL, 16);
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

    static const char *args[] = {"i64"};
    xrt_type_set_generic_origin(tid, tid);
    xrt_type_set_generic_name(tid, "Box<i64>", args, 1);
    ASSERT_TRUE_MSG(hot->generic_origin == tid, "generic origin stays in hot row");
    ASSERT_CSTR(xrt_type_display_name(tid), "Box<i64>", "display name comes from name row");
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
    test_xrt_exact_number_parse_failure_channel();
    test_xrt_to_bool_reuses_truthy_core_for_scalars_and_strings();
    test_xrt_to_bool_reuses_truthy_core_for_sized_containers();
    test_xrt_type_identity_uses_stable_owner_adapter();
    test_map_entries_dispatch_returns_tuple_array();
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
