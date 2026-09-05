/* generated-native VM ABI parity and fixed-cost gate. */

#include "../../../src/os/os_time.h"
#include "../../../src/module/xmodule.h"
#include "../../../src/runtime/object/xstring.h"
#include "../../../src/runtime/object/xarray.h"
#include "../../../src/runtime/class/xenum.h"
#include "../../../src/runtime/mem/xcoro_heap.h"
#include "../../../src/runtime/xisolate_api.h"
#include "../../../src/runtime/xexec_frame.h"
#include "../../../src/runtime/value/xvalue.h"
#include "../../../src/stdlib/xstdlib_vm_fastpath.h"
#include "../test_helper.h"
#include "xray_vm.h"
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define ASSERT_TRUE(expr)                                                                          \
    do {                                                                                           \
        if (!(expr)) {                                                                             \
            fprintf(stderr, "assertion failed at %s:%d: %s\n", __FILE__, __LINE__, #expr);         \
            return 1;                                                                              \
        }                                                                                          \
    } while (0)

#if defined(_MSC_VER)
#define XR_TEST_NOINLINE __declspec(noinline)
#elif defined(__GNUC__) || defined(__clang__)
#define XR_TEST_NOINLINE __attribute__((noinline))
#else
#define XR_TEST_NOINLINE
#endif

/* Migration-only C oracles. They are deliberately test-local and are not
 * linked into the VM or registered as alternate stdlib implementations. */
static XR_TEST_NOINLINE int legacy_http_redirect_status(int64_t status) {
    return status == 301 || status == 302 || status == 303 || status == 307 || status == 308;
}

static XR_TEST_NOINLINE int legacy_ws_valid_close_code(int64_t code) {
    return code == 1000 || code == 1001 || code == 1002 || code == 1003 || code == 1007 ||
           code == 1008 || code == 1009 || code == 1010 || code == 1011 ||
           (code >= 3000 && code <= 4999);
}

static XrValue legacy_http_adapter(XrVMRuntime *isolate, XrValue *args, int nargs) {
    (void) isolate;
    return xr_bool(nargs == 1 && XR_IS_INT(args[0]) &&
                   legacy_http_redirect_status(XR_TO_INT(args[0])));
}

static XrValue legacy_ws_adapter(XrVMRuntime *isolate, XrValue *args, int nargs) {
    (void) isolate;
    return xr_bool(nargs == 1 && XR_IS_INT(args[0]) &&
                   legacy_ws_valid_close_code(XR_TO_INT(args[0])));
}

static int compare_u64(const void *left, const void *right) {
    uint64_t a = *(const uint64_t *) left;
    uint64_t b = *(const uint64_t *) right;
    return a < b ? -1 : a > b ? 1 : 0;
}

static uint64_t measure(XrStdlibVmFastpathFn entry, int64_t seed, uint64_t *sink) {
    const int iterations = 2000000;
    XrValue arg = xr_int(seed);
    uint64_t start = xr_time_monotonic_ns();
    uint64_t local = 0;
    for (int i = 0; i < iterations; i++) {
        arg.i = seed + (i & 4095);
        XrValue result = entry(NULL, &arg, 1);
        local += (uint64_t) XR_TO_BOOL(result);
    }
    uint64_t elapsed = xr_time_monotonic_ns() - start;
    *sink += local;
    return elapsed;
}

static int verify_function(const char *module, const char *member, XrStdlibVmFastpathFn legacy,
                           int64_t begin, int64_t end) {
    XrStdlibVmFastpathFn generated = xr_stdlib_vm_fastpath_lookup(module, member);
    ASSERT_TRUE(generated != NULL);
    for (int64_t value = begin; value <= end; value++) {
        XrValue arg = xr_int(value);
        XrValue actual = generated(NULL, &arg, 1);
        XrValue expected = legacy(NULL, &arg, 1);
        ASSERT_TRUE(XR_IS_BOOL(actual));
        ASSERT_TRUE(XR_TO_BOOL(actual) == XR_TO_BOOL(expected));
    }
    return 0;
}

static int verify_runtime_overlay(void) {
    XrVMConfig config = {0};
    XrVMRuntime *isolate = xray_vm_new_full(&config);
    ASSERT_TRUE(isolate != NULL);

    /* Only modules that stay eligible for hosting export a C function here;
     * a module that reaches its private native primitives keeps its Xray
     * export. */
    const char *modules[] = {"cluster", "codegen", "datetime", "http", "text", "ws"};
    const char *members[] = {"validNodeName",    "compilerFence", "offset",
                             "isRedirectStatus", "trim",          "isValidCloseCode"};
    for (size_t i = 0; i < sizeof(modules) / sizeof(modules[0]); i++) {
        XrValue module_value = xr_module_import(isolate, modules[i]);
        ASSERT_TRUE(xr_value_is_module(module_value));
        XrModule *module = xr_value_to_module(module_value);
        XrValue exported = xr_module_get_export(isolate, module, members[i]);
        ASSERT_TRUE(xr_value_is_cfunction(exported));
    }
    xray_vm_delete(isolate);
    return 0;
}

static int verify_yieldable_runtime_overlay(void) {
    XrVMConfig config = {0};
    XrVMRuntime *isolate = xray_vm_new_full(&config);
    ASSERT_TRUE(isolate != NULL);
    /* os keeps its Xray exports now that a module reaching private native
     * primitives stays interpreted, and no module that is still eligible for
     * hosting declares a public yieldable entry, so there is nothing to
     * observe the yieldable overlay on. */
    XrValue module_value = xr_module_import(isolate, "os");
    ASSERT_TRUE(xr_value_is_module(module_value));
    XrValue exported = xr_module_get_export(isolate, xr_value_to_module(module_value), "sleep");
    if (!xr_value_is_cfunction(exported)) {
        xray_vm_delete(isolate);
        return 0;
    }
    ASSERT_TRUE(xr_value_to_cfunction(exported)->is_yieldable);
    xray_vm_delete(isolate);
    return 0;
}

static int verify_string_boundary(void) {
    XrVMConfig config = {0};
    XrVMRuntime *isolate = xray_vm_new_full(&config);
    ASSERT_TRUE(isolate != NULL);
    ASSERT_TRUE(xr_test_init_coro(isolate) != NULL);
    ASSERT_TRUE(xr_value_is_module(xr_module_import(isolate, "text")));
    ASSERT_TRUE(xr_value_is_module(xr_module_import(isolate, "cluster")));

    static const char source[] = "  h\xc3\xa9llo \n";
    static const char expected[] = "h\xc3\xa9llo";
    XrString *input = xr_string_new(isolate, source, sizeof(source) - 1);
    ASSERT_TRUE(input != NULL);
    XrValue argument = xr_string_value(input);
    XrStdlibVmFastpathFn trim = xr_stdlib_vm_fastpath_lookup("text", "trim");
    ASSERT_TRUE(trim != NULL);
    XrValue result = trim(isolate, &argument, 1);
    ASSERT_TRUE(XR_IS_STRING(result));
    XrString *output = XR_TO_STRING(result);
    ASSERT_TRUE(output != input);
    ASSERT_TRUE(output->length == sizeof(expected) - 1);
    ASSERT_TRUE(output->rune_length == 5);
    ASSERT_TRUE(memcmp(output->data, expected, sizeof(expected) - 1) == 0);

    /* cluster is source-only, so its public validation helper is eligible for
     * the hosted string boundary. */
    XrStdlibVmFastpathFn valid_name = xr_stdlib_vm_fastpath_lookup("cluster", "validNodeName");
    ASSERT_TRUE(valid_name != NULL);
    XrString *node = xr_string_new(isolate, "node-01", 7);
    ASSERT_TRUE(node != NULL);
    XrValue node_argument = xr_string_value(node);
    XrValue valid = valid_name(isolate, &node_argument, 1);
    ASSERT_TRUE(XR_IS_BOOL(valid));
    ASSERT_TRUE(XR_TO_BOOL(valid));

    xray_vm_delete(isolate);
    return 0;
}

static int verify_byte_array_boundary(void) {
    XrVMConfig config = {0};
    XrVMRuntime *isolate = xray_vm_new_full(&config);
    ASSERT_TRUE(isolate != NULL);
    XrCoroutine *coro = xr_test_init_coro(isolate);
    ASSERT_TRUE(coro != NULL);
    ASSERT_TRUE(xr_value_is_module(xr_module_import(isolate, "encoding")));

    XrArray *bytes = xr_array_with_capacity_typed(coro, 3, XR_ELEM_U8);
    ASSERT_TRUE(bytes != NULL);
    bytes->length = 3;
    ((uint8_t *) bytes->data)[0] = 0x00;
    ((uint8_t *) bytes->data)[1] = 0xab;
    ((uint8_t *) bytes->data)[2] = 0xff;
    XrValue bytes_argument = xr_make_ptr_val(bytes);

    XrStdlibVmFastpathFn encode = xr_stdlib_vm_fastpath_lookup("encoding", "hexEncode");
    ASSERT_TRUE(encode != NULL);
    XrValue encoded = encode(isolate, &bytes_argument, 1);
    ASSERT_TRUE(XR_IS_STRING(encoded));
    XrString *encoded_string = XR_TO_STRING(encoded);
    ASSERT_TRUE(encoded_string->length == 6);
    ASSERT_TRUE(memcmp(encoded_string->data, "00abff", 6) == 0);

    XrSliceView readonly_view = {bytes->data, bytes->length};
    XrValue readonly_argument =
        xr_span_ref_typed(&readonly_view, XR_ELEM_U8, 1, 0, 0, XR_SLICE_VIEW_READONLY);
    XrValue encoded_view = encode(isolate, &readonly_argument, 1);
    ASSERT_TRUE(XR_IS_STRING(encoded_view));
    XrString *encoded_view_string = XR_TO_STRING(encoded_view);
    ASSERT_TRUE(encoded_view_string->length == 6);
    ASSERT_TRUE(memcmp(encoded_view_string->data, "00abff", 6) == 0);

    XrStdlibVmFastpathFn decode = xr_stdlib_vm_fastpath_lookup("encoding", "hexDecode");
    ASSERT_TRUE(decode != NULL);
    XrValue encoded_argument = encoded;
    XrValue decoded = decode(isolate, &encoded_argument, 1);
    ASSERT_TRUE(XR_IS_ARRAY(decoded));
    XrArray *decoded_array = (XrArray *) decoded.ptr;
    ASSERT_TRUE((decoded_array->hdr.extra & XR_OBJ_AOT_NATIVE) != 0);
    ASSERT_TRUE(decoded_array->elem_type == XR_ELEM_U8);
    ASSERT_TRUE(decoded_array->length == 3);
    ASSERT_TRUE(((uint8_t *) decoded_array->data)[0] == 0x00);
    ASSERT_TRUE(((uint8_t *) decoded_array->data)[1] == 0xab);
    ASSERT_TRUE(((uint8_t *) decoded_array->data)[2] == 0xff);

    xr_rc_release_value(xr_coro_get_heap(coro), decoded);

    XrString *odd_hex = xr_string_new(isolate, "0", 1);
    ASSERT_TRUE(odd_hex != NULL);
    XrValue odd_argument = xr_string_value(odd_hex);
    XrValue rejected = decode(isolate, &odd_argument, 1);
    ASSERT_TRUE(XR_IS_NULL(rejected));
    XrVMContext *vm_ctx = xr_isolate_get_vm_ctx(isolate);
    ASSERT_TRUE(vm_ctx != NULL);
    ASSERT_TRUE(xr_value_is_enum_aggregate(vm_ctx->pending_error));
    XrEnumAggregateValue *error = xr_value_to_enum_aggregate(vm_ctx->pending_error);
    ASSERT_TRUE(error != NULL);
    ASSERT_TRUE(strcmp(xr_enum_aggregate_type(error)->name, "HexError") == 0);
    ASSERT_TRUE(strcmp(xr_enum_aggregate_member_name(error), "OddLength") == 0);
    XrValue length = xr_enum_aggregate_payload_get(error, 0);
    ASSERT_TRUE(XR_IS_INT(length) && XR_TO_INT(length) == 1);
    xr_rc_release_value(xr_coro_get_heap(coro), vm_ctx->pending_error);
    vm_ctx->pending_error = xr_null();

    /* The bridge consumes the AOT TLS error exactly once.  A subsequent
     * successful call proves no stale error can leak across VM invocations. */
    decoded = decode(isolate, &encoded_argument, 1);
    ASSERT_TRUE(XR_IS_ARRAY(decoded));
    ASSERT_TRUE(XR_IS_NULL(vm_ctx->pending_error));
    xr_rc_release_value(xr_coro_get_heap(coro), decoded);
    xray_vm_delete(isolate);
    return 0;
}

static int verify_scalar_array_boundary(void) {
    XrVMConfig config = {0};
    XrVMRuntime *isolate = xray_vm_new_full(&config);
    ASSERT_TRUE(isolate != NULL);
    XrCoroutine *coro = xr_test_init_coro(isolate);
    ASSERT_TRUE(coro != NULL);
    ASSERT_TRUE(xr_value_is_module(xr_module_import(isolate, "os")));
    /* os stays interpreted for the same reason, so the native-array overlay
     * has no hosted os entry to observe here. */
    XrStdlibVmFastpathFn loadavg = xr_stdlib_vm_fastpath_lookup("os", "loadavg");
    if (!loadavg) {
        xray_vm_delete(isolate);
        return 0;
    }
    XrValue result = loadavg(isolate, NULL, 0);
    ASSERT_TRUE(XR_IS_ARRAY(result));
    XrArray *array = (XrArray *) result.ptr;
    ASSERT_TRUE((array->hdr.extra & XR_OBJ_AOT_NATIVE) != 0);
    ASSERT_TRUE(array->elem_type == XR_ELEM_F64);
    ASSERT_TRUE(array->length == 3);
    xr_rc_release_value(xr_coro_get_heap(coro), result);
    xray_vm_delete(isolate);
    return 0;
}

static int verify_deferred_log_state_boundary(void) {
    XrVMConfig config = {0};
    XrVMRuntime *isolate = xray_vm_new_full(&config);
    ASSERT_TRUE(isolate != NULL);
    ASSERT_TRUE(xr_test_init_coro(isolate) != NULL);
    XrValue module_value = xr_module_import(isolate, "log");
    ASSERT_TRUE(xr_value_is_module(module_value));
    XrModule *module = xr_value_to_module(module_value);
    XrValue level_type_value = xr_module_get_export(isolate, module, "LogLevel");
    ASSERT_TRUE(XR_IS_ENUM_TYPE(level_type_value));
    XrEnumType *level_type = XR_TO_ENUM_TYPE(level_type_value);
    ASSERT_TRUE(level_type != NULL);
    ASSERT_TRUE(xr_stdlib_vm_fastpath_lookup("log", "setLevel") == NULL);
    ASSERT_TRUE(xr_stdlib_vm_fastpath_lookup("log", "isEnabled") == NULL);
    xray_vm_delete(isolate);
    return 0;
}

int main(void) {
    ASSERT_TRUE(XR_HOSTED_OBJECT_ABI_VERSION == 1);
    ASSERT_TRUE(XR_HOSTED_FRAGMENT_ABI_VERSION == 7);
    ASSERT_TRUE(sizeof(XrValue) == 16);
    ASSERT_TRUE(xr_stdlib_vm_fastpath_count() >= 40);
    ASSERT_TRUE(xr_stdlib_vm_fastpath_lookup("codegen", "compilerFence") != NULL);
    ASSERT_TRUE(xr_stdlib_vm_fastpath_lookup("text", "trim") != NULL);
    /* A module whose Xray source reaches its private native primitives keeps
     * every entry point on the interpreted path: a freestanding fragment
     * cannot link those primitives, and an opaque hosted proxy would strip the
     * VM fields a native binding reads. That covers the whole system surface,
     * suspendable and synchronous alike. */
    ASSERT_TRUE(xr_stdlib_vm_fastpath_lookup("net", "hasTLS") == NULL);
    ASSERT_TRUE(xr_stdlib_vm_fastpath_lookup("os", "getpid") == NULL);
    ASSERT_TRUE(xr_stdlib_vm_fastpath_lookup("os", "clock") == NULL);
    ASSERT_TRUE(xr_stdlib_vm_fastpath_lookup("os", "sleep") == NULL);
    if (verify_function("http", "isRedirectStatus", legacy_http_adapter, 0, 5999) != 0)
        return 1;
    if (verify_function("ws", "isValidCloseCode", legacy_ws_adapter, 0, 5999) != 0)
        return 1;
    if (verify_runtime_overlay() != 0)
        return 1;
    if (verify_yieldable_runtime_overlay() != 0)
        return 1;
    if (verify_string_boundary() != 0)
        return 1;
    if (verify_byte_array_boundary() != 0)
        return 1;
    if (verify_scalar_array_boundary() != 0)
        return 1;
    if (verify_deferred_log_state_boundary() != 0)
        return 1;

#if defined(XR_BUILD_ASAN) || defined(XR_BUILD_TSAN)
    return 0;
#else
    XrStdlibVmFastpathFn generated[] = {
        xr_stdlib_vm_fastpath_lookup("http", "isRedirectStatus"),
        xr_stdlib_vm_fastpath_lookup("ws", "isValidCloseCode"),
    };
    XrStdlibVmFastpathFn legacy[] = {legacy_http_adapter, legacy_ws_adapter};
    const char *names[] = {"http.isRedirectStatus", "ws.isValidCloseCode"};
    volatile uint64_t sink = 0;
    for (size_t function = 0; function < 2; function++) {
        uint64_t generated_samples[9];
        uint64_t legacy_samples[9];
        for (size_t sample = 0; sample < 9; sample++) {
            uint64_t sample_sink = 0;
            /* Alternate order so thermal drift cannot systematically favor a path. */
            if ((sample & 1u) == 0) {
                generated_samples[sample] = measure(generated[function], 0, &sample_sink);
                legacy_samples[sample] = measure(legacy[function], 0, &sample_sink);
            } else {
                legacy_samples[sample] = measure(legacy[function], 0, &sample_sink);
                generated_samples[sample] = measure(generated[function], 0, &sample_sink);
            }
            sink += sample_sink;
        }
        qsort(generated_samples, 9, sizeof(uint64_t), compare_u64);
        qsort(legacy_samples, 9, sizeof(uint64_t), compare_u64);
        double generated_ns = (double) generated_samples[4] / 2000000.0;
        double legacy_ns = (double) legacy_samples[4] / 2000000.0;
        double ratio = legacy_ns > 0.0 ? generated_ns / legacy_ns : 999.0;
        double delta = generated_ns - legacy_ns;
        printf("%s generated=%.3fns legacy_c=%.3fns delta=%.3fns ratio=%.3f\n", names[function],
               generated_ns, legacy_ns, delta, ratio);
        /* Both paths already include the identical VM CFunction ABI adapter.
         * The remaining generated-call delta must stay a tiny fixed cost. */
        ASSERT_TRUE(ratio <= 1.35);
        ASSERT_TRUE(delta <= 2.0);
    }
    ASSERT_TRUE(sink != UINT64_C(0));
    return 0;
#endif
}
