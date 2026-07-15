#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#define XRT_DATA_ALIGN 32

static int g_passed;
static int g_failed;

static void *test_alloc_aligned(size_t size) {
    void *ptr = NULL;
    return posix_memalign(&ptr, XRT_DATA_ALIGN, size) == 0 ? ptr : NULL;
}

#define XRT_MALLOC(sz) malloc(sz)
#define XRT_CALLOC(n, sz) calloc((n), (sz))
#define XRT_REALLOC(p, sz) realloc((p), (sz))
#define XRT_FREE(p) free(p)
#define XRT_ALLOC_ALIGNED(sz) test_alloc_aligned(sz)
#define XRT_FREE_ALIGNED(p) free(p)

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

static void destroy_object(XrValue value) {
    if (value.tag != XR_TAG_PTR || !value.ptr || value.heap_type != 0)
        return;
    xrt_json_t *object = (xrt_json_t *) value.ptr;
    XRT_FREE((void *) object->field_names);
    XRT_FREE(object);
}

static XrValue user_json(void) {
    static const char *names[] = {"name", "age", "active", "score"};
    XrValue value = xrt_json_new_named(4, names);
    xrt_json_set_field(value, 0, xr_box_str("Ada"));
    xrt_json_set_field(value, 1, XR_FROM_INT(37));
    xrt_json_set_field(value, 2, XR_TRUE_VAL);
    xrt_json_set_field(value, 3, XR_FROM_FLOAT(9.5));
    return value;
}

static void test_decode_validates_each_primitive_field(void) {
    static const char *names[] = {"name", "age", "active", "score"};
    static const uint8_t kinds[] = {XR_JSON_VALUE_STRING, XR_JSON_VALUE_INT, XR_JSON_VALUE_BOOL,
                                    XR_JSON_VALUE_FLOAT};
    XrValue source = user_json();
    XrValue decoded = xrt_json_decode_record(source, 4, names, kinds);
    ASSERT_TRUE(!XR_IS_NULL(decoded), "valid typed Json should decode");
    ASSERT_TRUE(((xrt_json_t *) decoded.ptr)->object_kind == XRT_OBJECT_RECORD,
                "decode should construct a Record");
    destroy_object(decoded);

    xrt_json_set_field(source, 0, XR_FROM_INT(1));
    ASSERT_TRUE(XR_IS_NULL(xrt_json_decode_record(source, 4, names, kinds)),
                "string field must reject an integer");
    xrt_json_set_field(source, 0, xr_box_str("Ada"));
    xrt_json_set_field(source, 1, XR_FROM_FLOAT(37.0));
    ASSERT_TRUE(XR_IS_NULL(xrt_json_decode_record(source, 4, names, kinds)),
                "int field must reject a float");
    xrt_json_set_field(source, 1, XR_FROM_INT(37));
    xrt_json_set_field(source, 2, XR_FROM_INT(1));
    ASSERT_TRUE(XR_IS_NULL(xrt_json_decode_record(source, 4, names, kinds)),
                "bool field must reject an integer");
    xrt_json_set_field(source, 2, XR_TRUE_VAL);
    xrt_json_set_field(source, 3, XR_FROM_INT(9));
    ASSERT_TRUE(XR_IS_NULL(xrt_json_decode_record(source, 4, names, kinds)),
                "float field must reject an integer");
    destroy_object(source);
}

static void test_decode_distinguishes_nullable_and_missing(void) {
    static const char *names[] = {"name"};
    const uint8_t nullable[] = {XR_JSON_VALUE_STRING | XR_JSON_VALUE_NULLABLE};
    const uint8_t required[] = {XR_JSON_VALUE_STRING};
    XrValue source = xrt_json_new_named(1, names);
    xrt_json_set_field(source, 0, XR_NULL_VAL);

    XrValue decoded = xrt_json_decode_record(source, 1, names, nullable);
    ASSERT_TRUE(!XR_IS_NULL(decoded), "nullable string field should accept null");
    destroy_object(decoded);
    ASSERT_TRUE(XR_IS_NULL(xrt_json_decode_record(source, 1, names, required)),
                "required string field should reject null");

    static const char *missing_names[] = {"other"};
    XrValue missing = xrt_json_new_named(1, missing_names);
    ASSERT_TRUE(XR_IS_NULL(xrt_json_decode_record(missing, 1, names, nullable)),
                "nullable does not make a missing field optional");
    destroy_object(missing);
    destroy_object(source);
}

static void test_decode_json_field_accepts_null(void) {
    static const char *names[] = {"payload"};
    static const uint8_t kinds[] = {XR_JSON_VALUE_JSON};
    XrValue source = xrt_json_new_named(1, names);
    xrt_json_set_field(source, 0, XR_NULL_VAL);

    XrValue decoded = xrt_json_decode_record(source, 1, names, kinds);
    ASSERT_TRUE(!XR_IS_NULL(decoded), "Json includes null without a redundant nullable wrapper");
    destroy_object(decoded);
    destroy_object(source);
}

int main(void) {
    test_decode_validates_each_primitive_field();
    test_decode_distinguishes_nullable_and_missing();
    test_decode_json_field_accepts_null();
    printf("test_xrt_json_decode: %d passed, %d failed\n", g_passed, g_failed);
    return g_failed == 0 ? 0 : 1;
}
