#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include "../test_win_compat.h"

#define XRT_DATA_ALIGN 32

static int g_passed;
static int g_failed;
static size_t g_allocations;
static size_t g_frees;

static void *test_malloc(size_t size) {
    g_allocations++;
    return malloc(size);
}

static void *test_calloc(size_t count, size_t size) {
    g_allocations++;
    return calloc(count, size);
}

static void test_free(void *ptr) {
    if (ptr)
        g_frees++;
    free(ptr);
}

static void *test_alloc_aligned(size_t size) {
    return xr_test_alloc_aligned(size, XRT_DATA_ALIGN);
}

#define XRT_MALLOC(sz) test_malloc(sz)
#define XRT_CALLOC(n, sz) test_calloc((n), (sz))
#define XRT_REALLOC(p, sz) realloc((p), (sz))
#define XRT_FREE(p) test_free(p)
#define XRT_ALLOC_ALIGNED(sz) test_alloc_aligned(sz)
#define XRT_FREE_ALIGNED(p) xr_test_free_aligned(p)

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
    xrt_release(value);
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

typedef struct TestStaticShape {
    XrtObjectShape shape;
    XrtObjectShapeField *fields;
    struct TestStaticShape *next;
} TestStaticShape;

static TestStaticShape *g_test_shapes;

static const XrtObjectShape *test_static_shape(int64_t field_count,
                                               const char *const *field_names) {
    for (TestStaticShape *entry = g_test_shapes; entry; entry = entry->next) {
        if (entry->shape.field_count != field_count)
            continue;
        int same = 1;
        for (int64_t i = 0; i < field_count; i++) {
            if (strcmp(entry->fields[i].name, field_names[i]) != 0) {
                same = 0;
                break;
            }
        }
        if (same)
            return &entry->shape;
    }

    TestStaticShape *entry = (TestStaticShape *) calloc(1, sizeof(TestStaticShape));
    if (!entry)
        abort();
    entry->fields =
        (XrtObjectShapeField *) calloc((size_t) field_count, sizeof(XrtObjectShapeField));
    if (!entry->fields)
        abort();
    for (int64_t i = 0; i < field_count; i++) {
        entry->fields[i].name = field_names[i];
        entry->fields[i].symbol_hash =
            xr_hash_bytes(field_names[i], strlen(field_names[i]));
        entry->fields[i].ordinal = (uint16_t) i;
    }
    entry->shape.stable_key =
        xr_object_shape_stable_key(XRT_OBJECT_STRUCT, entry->fields, field_count);
    entry->shape.field_count = field_count;
    entry->shape.fields = entry->fields;
    entry->shape.object_domain = XRT_OBJECT_STRUCT;
    entry->shape.storage = XR_OBJECT_SHAPE_STATIC;
    entry->next = g_test_shapes;
    g_test_shapes = entry;
    return &entry->shape;
}

static void destroy_test_static_shapes(void) {
    while (g_test_shapes) {
        TestStaticShape *next = g_test_shapes->next;
        free(g_test_shapes->fields);
        free(g_test_shapes);
        g_test_shapes = next;
    }
}

static void test_static_shape_is_zero_copy_and_header_shrinks(void) {
    static const XrtObjectShapeField fields[] = {
        {"x", UINT64_C(0x1), UINT32_C(0xfd0c5087), 0, 0, 0},
    };
    static const XrtObjectShape shape = {
        UINT64_C(0x1), 1, fields, XRT_OBJECT_STRUCT, XR_OBJECT_SHAPE_STATIC, 0, 0};
    size_t allocations_before = g_allocations;
    size_t frees_before = g_frees;
    XrValue object = xrt_object_new_shape(&shape);
    xrt_json_t *raw = (xrt_json_t *) object.ptr;

    ASSERT_TRUE(sizeof(xrt_json_t) <= 40, "descriptor object header should shrink to at most 40B");
    ASSERT_TRUE(g_allocations == allocations_before + 1,
                "static shape construction should allocate only the object body");
    ASSERT_TRUE(raw->shape == &shape, "object should borrow the file-static descriptor");
    ASSERT_TRUE(raw->dynamic_fields == NULL,
                "structural object construction should not allocate an extension map");
    ASSERT_TRUE(xrt_json_find_field(raw, "x") == 0, "descriptor name should map to ordinal zero");
    destroy_object(object);
    ASSERT_TRUE(g_frees == frees_before + 1,
                "destruction should free the object allocation but not the static descriptor");
}

static void test_clone_and_storage_keep_static_shape(void) {
    static const XrtObjectShapeField fields[] = {
        {"value", UINT64_C(0x2), UINT32_C(0x425ed3ca), 0, 0, 0},
    };
    static const XrtObjectShape shape = {
        UINT64_C(0x2), 1, fields, XRT_OBJECT_STRUCT, XR_OBJECT_SHAPE_STATIC, 0, 0};
    XrValue source = xrt_object_new_shape(&shape);
    xrt_json_set_field(source, 0, XR_FROM_INT(42));
    size_t allocations_before = g_allocations;
    XrValue clone = xrt_json_clone_for_coro(source);

    ASSERT_TRUE(g_allocations == allocations_before + 1,
                "cloning a static-shape object should allocate only the clone body");
    ASSERT_TRUE(((xrt_json_t *) clone.ptr)->shape == &shape,
                "clone should share the process-lifetime descriptor");
    ASSERT_TRUE(XR_TO_INT(xrt_json_get_field(clone, 0)) == 42,
                "clone should preserve the fixed field value");
    (void) xrt_json_set_storage(clone, XR_OBJ_STORAGE_NORMAL);
    ASSERT_TRUE(((xrt_json_t *) clone.ptr)->shape == &shape,
                "storage promotion should not replace or own the descriptor");
    destroy_object(clone);
    destroy_object(source);
}

static void test_shape_guard_checks_table_after_key_hit(void) {
    static const XrtObjectShapeField fields[] = {
        {"actual", UINT64_C(0x22), 0, 0, XR_OBJECT_SHAPE_FIELD_READONLY, 0},
    };
    XrtObjectShapeField runtime_field = fields[0];
    runtime_field.symbol_hash = xr_hash_bytes(runtime_field.name, strlen(runtime_field.name));
    XrtObjectShape shape = {UINT64_C(0xfeed), 1, &runtime_field, XRT_OBJECT_STRUCT,
                            XR_OBJECT_SHAPE_STATIC, 0, 0};
    XrValue object = xrt_object_new_shape(&shape);

    ASSERT_TRUE(xrt_object_shape_matches_key(object, UINT64_C(0xfeed), XRT_OBJECT_STRUCT),
                "forced stable-key hit should reach the descriptor-table guard");
    ASSERT_TRUE(!xrt_object_shape_field_matches_fingerprint(
                    object, 0, xr_object_shape_stable_name_key("collision"),
                    xr_hash_bytes("collision", strlen("collision")), UINT64_C(0x22),
                    XR_OBJECT_SHAPE_FIELD_READONLY),
                "a stable-key collision must not accept a different field table");
    ASSERT_TRUE(xrt_object_shape_field_matches_fingerprint(
                    object, 0, xr_object_shape_stable_name_key("actual"),
                    xr_hash_bytes("actual", strlen("actual")), UINT64_C(0x22),
                    XR_OBJECT_SHAPE_FIELD_READONLY),
                "the exact descriptor fingerprint should match");
    destroy_object(object);
}

static XrValue decode_with_kinds(XrValue data, int64_t field_count, const char *const *names,
                                 const uint8_t *kinds) {
    XrJsonDecodeFieldSpec *fields =
        (XrJsonDecodeFieldSpec *) calloc((size_t) field_count, sizeof(XrJsonDecodeFieldSpec));
    if (!fields)
        abort();
    for (int64_t i = 0; i < field_count; i++) {
        fields[i].name = names[i];
        fields[i].value_kind = kinds[i];
    }
    XrValue decoded =
        xrt_json_decode_struct_object(data, test_static_shape(field_count, names), field_count, fields);
    free(fields);
    return decoded;
}

static void test_decode_validates_each_primitive_field(void) {
    static const char *names[] = {"name", "age", "active", "score"};
    static const uint8_t kinds[] = {XR_JSON_VALUE_STRING, XR_JSON_VALUE_INT, XR_JSON_VALUE_BOOL,
                                    XR_JSON_VALUE_FLOAT};
    XrValue source = user_json();
    XrValue decoded = decode_with_kinds(source, 4, names, kinds);
    ASSERT_TRUE(!XR_IS_NULL(decoded), "valid typed Json should decode");
    ASSERT_TRUE(xrt_object_domain((xrt_json_t *) decoded.ptr) == XRT_OBJECT_STRUCT,
                "decode should construct a structural object");
    destroy_object(decoded);

    xrt_json_set_field(source, 0, XR_FROM_INT(1));
    ASSERT_TRUE(XR_IS_NULL(decode_with_kinds(source, 4, names, kinds)),
                "string field must reject an integer");
    xrt_json_set_field(source, 0, xr_box_str("Ada"));
    xrt_json_set_field(source, 1, XR_FROM_FLOAT(37.0));
    ASSERT_TRUE(XR_IS_NULL(decode_with_kinds(source, 4, names, kinds)),
                "int field must reject a float");
    xrt_json_set_field(source, 1, XR_FROM_INT(37));
    xrt_json_set_field(source, 2, XR_FROM_INT(1));
    ASSERT_TRUE(XR_IS_NULL(decode_with_kinds(source, 4, names, kinds)),
                "bool field must reject an integer");
    xrt_json_set_field(source, 2, XR_TRUE_VAL);
    xrt_json_set_field(source, 3, XR_FROM_INT(9));
    ASSERT_TRUE(XR_IS_NULL(decode_with_kinds(source, 4, names, kinds)),
                "float field must reject an integer");
    destroy_object(source);
}

static void test_decode_distinguishes_nullable_and_missing(void) {
    static const char *names[] = {"name"};
    const uint8_t nullable[] = {XR_JSON_VALUE_STRING | XR_JSON_VALUE_NULLABLE};
    const uint8_t required[] = {XR_JSON_VALUE_STRING};
    XrValue source = xrt_json_new_named(1, names);
    xrt_json_set_field(source, 0, XR_NULL_VAL);

    XrValue decoded = decode_with_kinds(source, 1, names, nullable);
    ASSERT_TRUE(!XR_IS_NULL(decoded), "nullable string field should accept null");
    destroy_object(decoded);
    ASSERT_TRUE(XR_IS_NULL(decode_with_kinds(source, 1, names, required)),
                "required string field should reject null");

    static const char *missing_names[] = {"other"};
    XrValue missing = xrt_json_new_named(1, missing_names);
    ASSERT_TRUE(XR_IS_NULL(decode_with_kinds(missing, 1, names, nullable)),
                "nullable does not make a missing field optional");
    destroy_object(missing);
    destroy_object(source);
}

static void test_decode_json_field_accepts_null(void) {
    static const char *names[] = {"payload"};
    static const uint8_t kinds[] = {XR_JSON_VALUE_JSON};
    XrValue source = xrt_json_new_named(1, names);
    xrt_json_set_field(source, 0, XR_NULL_VAL);

    XrValue decoded = decode_with_kinds(source, 1, names, kinds);
    ASSERT_TRUE(!XR_IS_NULL(decoded), "Json includes null without a redundant nullable wrapper");
    destroy_object(decoded);
    destroy_object(source);
}

static void test_decode_nested_object_field(void) {
    static const char *nested_names[] = {"id", "ok"};
    static const char *envelope_names[] = {"nested", "label"};
    const XrJsonDecodeFieldSpec nested_fields[] = {
        {"id", XR_JSON_VALUE_INT, NULL, 0, NULL},
        {"ok", XR_JSON_VALUE_BOOL, NULL, 0, NULL},
    };
    const XrJsonDecodeFieldSpec envelope_fields[] = {
        {"nested", XR_JSON_VALUE_STRUCT_OBJECT, nested_fields, 2,
         test_static_shape(2, nested_names)},
        {"label", XR_JSON_VALUE_STRING, NULL, 0, NULL},
    };

    XrValue nested = xrt_struct_object_new_named(2, nested_names);
    xrt_json_set_field(nested, 0, XR_FROM_INT(7));
    xrt_json_set_field(nested, 1, XR_TRUE_VAL);
    XrValue source = xrt_json_new_named(2, envelope_names);
    xrt_json_set_field(source, 0, nested);
    xrt_json_set_field(source, 1, xr_box_str("ok"));

    XrValue decoded = xrt_json_decode_struct_object(
        source, test_static_shape(2, envelope_names), 2, envelope_fields);
    ASSERT_TRUE(!XR_IS_NULL(decoded), "nested structural object field should decode");
    XrValue decoded_nested = xrt_json_get_field(decoded, 0);
    ASSERT_TRUE(decoded_nested.tag == XR_TAG_PTR && decoded_nested.ptr,
                "nested structural object decode should materialize a nested object");
    ASSERT_TRUE(xrt_object_domain((xrt_json_t *) decoded_nested.ptr) == XRT_OBJECT_STRUCT,
                "nested field should become a structural object");
    ASSERT_TRUE(XR_TO_INT(xrt_json_get_field(decoded_nested, 0)) == 7,
                "nested int field should be copied");
    destroy_object(decoded_nested);
    destroy_object(decoded);

    xrt_json_set_field(nested, 0, xr_box_str("bad"));
    ASSERT_TRUE(XR_IS_NULL(xrt_json_decode_struct_object(
                    source, test_static_shape(2, envelope_names), 2, envelope_fields)),
                "nested structural object validation should reject wrong nested primitive");
    destroy_object(nested);
    destroy_object(source);
}

static void test_decode_deep_nested_object_field(void) {
    static const char *geo_names[] = {"lat", "verified"};
    static const char *address_names[] = {"city", "geo"};
    static const char *profile_names[] = {"name", "address"};
    const XrJsonDecodeFieldSpec geo_fields[] = {
        {"lat", XR_JSON_VALUE_FLOAT, NULL, 0, NULL},
        {"verified", XR_JSON_VALUE_BOOL, NULL, 0, NULL},
    };
    const XrJsonDecodeFieldSpec address_fields[] = {
        {"city", XR_JSON_VALUE_STRING, NULL, 0, NULL},
        {"geo", XR_JSON_VALUE_STRUCT_OBJECT, geo_fields, 2, test_static_shape(2, geo_names)},
    };
    const XrJsonDecodeFieldSpec profile_fields[] = {
        {"name", XR_JSON_VALUE_STRING, NULL, 0, NULL},
        {"address", XR_JSON_VALUE_STRUCT_OBJECT, address_fields, 2,
         test_static_shape(2, address_names)},
    };

    XrValue geo = xrt_json_new_named(2, geo_names);
    xrt_json_set_field(geo, 0, XR_FROM_FLOAT(30.25));
    xrt_json_set_field(geo, 1, XR_TRUE_VAL);
    XrValue address = xrt_json_new_named(2, address_names);
    xrt_json_set_field(address, 0, xr_box_str("Hangzhou"));
    xrt_json_set_field(address, 1, geo);
    XrValue source = xrt_json_new_named(2, profile_names);
    xrt_json_set_field(source, 0, xr_box_str("Dana"));
    xrt_json_set_field(source, 1, address);

    XrValue decoded = xrt_json_decode_struct_object(
        source, test_static_shape(2, profile_names), 2, profile_fields);
    ASSERT_TRUE(!XR_IS_NULL(decoded), "deep nested structural object field should decode");
    XrValue decoded_address = xrt_json_get_field(decoded, 1);
    XrValue decoded_geo = xrt_json_get_field(decoded_address, 1);
    ASSERT_TRUE(xrt_object_domain((xrt_json_t *) decoded_address.ptr) == XRT_OBJECT_STRUCT,
                "second-level nested object should become a structural object");
    ASSERT_TRUE(xrt_object_domain((xrt_json_t *) decoded_geo.ptr) == XRT_OBJECT_STRUCT,
                "third-level nested object should become a structural object");
    ASSERT_TRUE(XR_IS_BOOL(xrt_json_get_field(decoded_geo, 1)) &&
                    XR_TO_BOOL(xrt_json_get_field(decoded_geo, 1)),
                "deep nested bool field should be copied");
    destroy_object(decoded_geo);
    destroy_object(decoded_address);
    destroy_object(decoded);

    xrt_json_set_field(geo, 1, xr_box_str("bad"));
    ASSERT_TRUE(XR_IS_NULL(xrt_json_decode_struct_object(
                    source, test_static_shape(2, profile_names), 2, profile_fields)),
                "deep nested validation should reject wrong leaf type");
    destroy_object(geo);
    destroy_object(address);
    destroy_object(source);
}

static void test_decode_validates_array_json_field(void) {
    static const char *names[] = {"items"};
    static const XrJsonDecodeFieldSpec item_schema[] = {
        {NULL, XR_JSON_VALUE_JSON, NULL, 0, NULL},
    };
    static const XrJsonDecodeFieldSpec fields[] = {
        {"items", XR_JSON_VALUE_ARRAY, item_schema, 1,
         (const XrtObjectShape *) (uintptr_t) XR_ELEM_ANY},
    };
    XrValue source = xrt_json_new_named(1, names);
    XrValue items = xrt_array_with_capacity(2);
    xrt_array_push(items, XR_FROM_INT(1));
    xrt_array_push(items, XR_TRUE_VAL);
    xrt_json_set_field(source, 0, items);

    XrValue decoded =
        xrt_json_decode_struct_object(source, test_static_shape(1, names), 1, fields);
    ASSERT_TRUE(!XR_IS_NULL(decoded), "Array<Json> field should accept a JSON array");
    destroy_object(decoded);

    xrt_json_set_field(source, 0, xr_box_str("not-array"));
    ASSERT_TRUE(XR_IS_NULL(
                    xrt_json_decode_struct_object(source, test_static_shape(1, names), 1, fields)),
                "Array<Json> field should reject a scalar");
    destroy_object(source);
}

static void test_decode_mixed_nested_object_and_array_json_fields(void) {
    static const char *nested_names[] = {"city", "zip"};
    static const char *source_names[] = {"name", "tags", "address", "active"};
    const XrJsonDecodeFieldSpec nested_fields[] = {
        {"city", XR_JSON_VALUE_STRING, NULL, 0, NULL},
        {"zip", XR_JSON_VALUE_INT, NULL, 0, NULL},
    };
    const XrJsonDecodeFieldSpec tag_item_schema[] = {
        {NULL, XR_JSON_VALUE_JSON, NULL, 0, NULL},
    };
    const XrJsonDecodeFieldSpec fields[] = {
        {"name", XR_JSON_VALUE_STRING, NULL, 0, NULL},
        {"tags", XR_JSON_VALUE_ARRAY, tag_item_schema, 1,
         (const XrtObjectShape *) (uintptr_t) XR_ELEM_ANY},
        {"address", XR_JSON_VALUE_STRUCT_OBJECT, nested_fields, 2,
         test_static_shape(2, nested_names)},
        {"active", XR_JSON_VALUE_BOOL, NULL, 0, NULL},
    };

    XrValue source = xrt_json_new_named(4, source_names);
    XrValue tags = xrt_array_with_capacity(3);
    xrt_array_push(tags, xr_box_str("ops"));
    xrt_array_push(tags, XR_FROM_INT(3));
    xrt_array_push(tags, XR_NULL_VAL);
    XrValue address = xrt_json_new_named(2, nested_names);
    xrt_json_set_field(address, 0, xr_box_str("Hangzhou"));
    xrt_json_set_field(address, 1, XR_FROM_INT(310000));
    xrt_json_set_field(source, 0, xr_box_str("Dana"));
    xrt_json_set_field(source, 1, tags);
    xrt_json_set_field(source, 2, address);
    xrt_json_set_field(source, 3, XR_TRUE_VAL);

    XrValue decoded = xrt_json_decode_struct_object(
        source, test_static_shape(4, source_names), 4, fields);
    ASSERT_TRUE(!XR_IS_NULL(decoded), "mixed typed Json should decode");
    XrValue decoded_active = xrt_json_get_field(decoded, 3);
    ASSERT_TRUE(XR_IS_BOOL(decoded_active) && XR_TO_BOOL(decoded_active),
                "bool field should be copied after nested structural object and Array<Json>");
    XrValue decoded_address = xrt_json_get_field(decoded, 2);
    ASSERT_TRUE(!XR_IS_NULL(decoded_address), "nested address should be materialized");
    ASSERT_TRUE(XR_TO_INT(xrt_json_get_field(decoded_address, 1)) == 310000,
                "nested int field should survive mixed decode");
    destroy_object(decoded_address);
    destroy_object(decoded);

    xrt_json_set_field(address, 1, xr_box_str("bad"));
    ASSERT_TRUE(XR_IS_NULL(xrt_json_decode_struct_object(
                    source, test_static_shape(4, source_names), 4, fields)),
                "mixed decode should reject wrong nested primitive");
    xrt_json_set_field(address, 1, XR_FROM_INT(310000));
    xrt_json_set_field(source, 1, xr_box_str("not-array"));
    ASSERT_TRUE(XR_IS_NULL(xrt_json_decode_struct_object(
                    source, test_static_shape(4, source_names), 4, fields)),
                "mixed decode should reject non-array Array<Json> field");
    destroy_object(address);
    destroy_object(source);
}

static int parse_typed_for_test(const char *text, const XrtObjectShape *shape,
                                int64_t field_count, const XrJsonDecodeFieldSpec *fields,
                                XrValue *out, XrtJsonTypedParseError *error) {
    xrt_json_parser_t parser = {
        .src = text,
        .end = text ? text + strlen(text) : text,
        .pos = text,
        .depth = 0,
    };
    memset(error, 0, sizeof(*error));
    *out = XR_NULL_VAL;
    int ok = text && xrt_json_parse_typed_object_value(
                         &parser, shape, field_count, fields, "$", out, error);
    if (ok) {
        xrt_json_parse_skip_ws(&parser);
        if (parser.pos != parser.end) {
            xrt_release(*out);
            *out = XR_NULL_VAL;
            xrt_json_typed_error(&parser, error, "$", "end_of_input", NULL);
            ok = 0;
        }
    }
    return ok;
}

static void test_parse_typed_direct_has_no_intermediate_dom(void) {
    static const char *names[] = {"count"};
    static const XrJsonDecodeFieldSpec fields[] = {
        {"count", XR_JSON_VALUE_INT, NULL, 0, NULL},
    };
    const XrtObjectShape *shape = test_static_shape(1, names);
    size_t baseline_allocations_before = g_allocations;
    size_t baseline_frees_before = g_frees;
    XrValue parsed = XR_NULL_VAL;
    XrtJsonTypedParseError error;

    ASSERT_TRUE(parse_typed_for_test("{\"count\":7}", shape, 1, fields, &parsed, &error),
                "typed parse baseline should construct the target object");
    destroy_object(parsed);
    size_t baseline_allocations = g_allocations - baseline_allocations_before;
    size_t baseline_frees = g_frees - baseline_frees_before;
    ASSERT_TRUE(baseline_allocations == baseline_frees,
                "typed parse baseline should release scratch and target allocations");

    size_t allocations_before = g_allocations;
    size_t frees_before = g_frees;
    ASSERT_TRUE(parse_typed_for_test(
                    "{\"ignored\":{\"deep\":[1,2,3]},\"count\":7}", shape, 1, fields,
                    &parsed, &error),
                "typed parse should skip unknown Json without materializing it");
    ASSERT_TRUE(g_allocations - allocations_before == baseline_allocations + 1,
                "ignored nested Json should allocate only its top-level key scratch");
    ASSERT_TRUE(XR_TO_INT(xrt_json_get_field(parsed, 0)) == 7,
                "typed parse should populate the canonical target slot");
    destroy_object(parsed);
    ASSERT_TRUE(g_frees - frees_before == g_allocations - allocations_before,
                "typed parse scratch and target allocations should all be released");
}

static void test_parse_typed_direct_reports_stable_errors_and_unwinds(void) {
    static const char *nested_names[] = {"label", "ok"};
    static const char *root_names[] = {"nested", "id"};
    static const XrJsonDecodeFieldSpec nested_fields[] = {
        {"label", XR_JSON_VALUE_STRING, NULL, 0, NULL},
        {"ok", XR_JSON_VALUE_BOOL, NULL, 0, NULL},
    };
    const XrJsonDecodeFieldSpec fields[] = {
        {"nested", XR_JSON_VALUE_STRUCT_OBJECT, nested_fields, 2,
         test_static_shape(2, nested_names)},
        {"id", XR_JSON_VALUE_INT, NULL, 0, NULL},
    };
    const XrtObjectShape *shape = test_static_shape(2, root_names);
    size_t allocations_before = g_allocations;
    size_t frees_before = g_frees;
    XrValue parsed = XR_NULL_VAL;
    XrtJsonTypedParseError error;

    ASSERT_TRUE(!parse_typed_for_test(
                    "{\"nested\":{\"label\":\"owned\",\"ok\":1},\"id\":7}", shape,
                    2, fields, &parsed, &error),
                "typed parse should reject a wrong nested token kind");
    ASSERT_TRUE(strcmp(error.path, "$.nested.ok") == 0,
                "typed parse should retain the precise nested Json path");
    ASSERT_TRUE(strcmp(error.expected, "bool") == 0 && strcmp(error.actual, "number") == 0,
                "typed parse should report stable expected/actual token kinds");
    ASSERT_TRUE(g_allocations - allocations_before == g_frees - frees_before,
                "typed parse failure should release every partial allocation");

    ASSERT_TRUE(!parse_typed_for_test(
                    "{\"nested\":{\"label\":\"x\",\"ok\":true},\"id\":7,\"id\":8}",
                    shape, 2, fields, &parsed, &error),
                "typed parse should reject duplicate target fields");
    ASSERT_TRUE(strcmp(error.path, "$.id") == 0 && strcmp(error.actual, "duplicate") == 0,
                "duplicate-field errors should identify the target path");

    ASSERT_TRUE(!parse_typed_for_test(
                    "{\"nested\":{\"label\":\"x\",\"ok\":true}}", shape, 2, fields,
                    &parsed, &error),
                "typed parse should reject missing required fields");
    ASSERT_TRUE(strcmp(error.path, "$.id") == 0 && strcmp(error.actual, "missing") == 0,
                "missing-field errors should identify the target path");
}

int main(void) {
    test_static_shape_is_zero_copy_and_header_shrinks();
    test_clone_and_storage_keep_static_shape();
    test_shape_guard_checks_table_after_key_hit();
    test_decode_validates_each_primitive_field();
    test_decode_distinguishes_nullable_and_missing();
    test_decode_json_field_accepts_null();
    test_decode_nested_object_field();
    test_decode_deep_nested_object_field();
    test_decode_validates_array_json_field();
    test_decode_mixed_nested_object_and_array_json_fields();
    test_parse_typed_direct_has_no_intermediate_dom();
    test_parse_typed_direct_reports_stable_errors_and_unwinds();
    destroy_test_static_shapes();
    printf("test_xrt_json_decode: %d passed, %d failed\n", g_passed, g_failed);
    return g_failed == 0 ? 0 : 1;
}
