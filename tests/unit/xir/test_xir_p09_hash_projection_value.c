/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_p09_hash_projection_value.c - Atomic ordered projection allocation
 */
#define XR_COMPILE_DOMAIN_ENTRY original_projection_compile_domain_main
#include "test_xir_compile_domain.c"
#undef XR_COMPILE_DOMAIN_ENTRY
static XrXirTypeArena *projection_value_arena(void) {
    XrXirCallableParameter fields[] = {{XR_XIR_I64, 0}, {XR_XIR_STRING, 0}};
    XrXirTypeNode nodes[] = {
        {.kind = XR_XIR_TYPE_MAP, .element = XR_XIR_I64, .result = XR_XIR_STRING},
        {.kind = XR_XIR_TYPE_SET, .element = XR_XIR_STRING},
        {.kind = XR_XIR_TYPE_ARRAY, .element = XR_XIR_I64},
        {.kind = XR_XIR_TYPE_ARRAY, .element = XR_XIR_STRING},
        {.kind = XR_XIR_TYPE_TUPLE, .parameters = fields, .parameter_count = 2},
        {.kind = XR_XIR_TYPE_ARRAY, .element = (XrXirType)260},
        {.kind = XR_XIR_TYPE_ARRAY, .element = XR_XIR_F64}
    };
    XrXirTypes types = {nodes, 7, NULL, NULL}; XrXirTypeArena *arena = NULL;
    CHECK(value_compile_arena(&types, 65536, value_compile_limits(1048576, 1048576, 100000), &arena) == XR_XIR_VALUE_OK);
    return arena;
}
static XrXirValueStatus projection_value_op(unsigned operation, const XrXirValue *source,
    XrXirValueAdmission *admission, XrXirValue *output) {
    if (!operation) return xr_xir_map_keys(source, (XrXirType)258, admission, output);
    if (operation == 2) return xr_xir_map_entries(source, (XrXirType)261, admission, output);
    return xr_xir_hash_values(source, (XrXirType)259, admission, output);
}
static void projection_value_check(XrXirValue *result, unsigned operation, XrXirValueAdmission *admission) {
    int64_t length = -1;
    CHECK(xr_xir_array_len(result, admission, &length) == XR_XIR_VALUE_OK && length == 2);
    const char *texts[] = {"first-owned-string", "second-owned-string"};
    for (uint32_t i = 0; i < 2; ++i) {
        XrXirValue item = {0}, key = {0}, field = {0}; XrXirFaultDetail fault = {0};
        CHECK(xr_xir_array_get(result, i, admission, &item, &fault) == XR_XIR_VALUE_OK);
        const XrXirValue *text = &item;
        if (operation == 2) {
            CHECK(xr_xir_tuple_get(&item, 0, &key) == XR_XIR_VALUE_OK && key.payload == (i ? 1 : 2));
            CHECK(xr_xir_tuple_get(&item, 1, &field) == XR_XIR_VALUE_OK); text = &field;
        }
        if (!operation) CHECK(item.type == XR_XIR_I64 && item.payload == (i ? 1 : 2));
        else {
            const char *bytes = NULL; size_t count = 0;
            CHECK(xr_xir_string_view(text, &bytes, &count) && count == strlen(texts[i]) && !memcmp(bytes, texts[i], count));
        }
        xr_xir_value_drop(&field); xr_xir_value_drop(&key); xr_xir_value_drop(&item);
    }
}
static void projection_value_faults(unsigned operation, const XrXirValue *source,
    XrXirValueAdmission *admission, bool full) {
    XrXirValue result = {0}; const uint64_t initial = 1000000;
    admission->work = initial; size_t before = calls, baseline = live;
    CHECK(projection_value_op(operation, source, admission, &result) == XR_XIR_VALUE_OK);
    size_t points = calls - before; uint64_t exact = initial - admission->work;
    CHECK(points && exact > 1); admission->work = initial;
    projection_value_check(&result, operation, admission); xr_xir_value_drop(&result); CHECK(live == baseline);
    XirHash *map = (XirHash *)object_pointer(source); size_t bytes = 0;
    CHECK(hash_data_size(map->capacity, map->stride, &bytes) && bytes <= 256);
    unsigned char saved[256]; memcpy(saved, map->data, bytes); int64_t payload = source->payload;
    if (full) for (size_t point = 0; point < points; ++point) {
        admission->work = initial; fail_at = calls + point;
        CHECK(projection_value_op(operation, source, admission, &result) == XR_XIR_VALUE_OOM && calls > fail_at);
        fail_at = SIZE_MAX;
        CHECK(!result.type && !result.reserved && !result.payload && live == baseline);
        CHECK(source->payload == payload && !memcmp(map->data, saved, bytes));
        admission->work = initial;
        CHECK(projection_value_op(operation, source, admission, &result) == XR_XIR_VALUE_OK);
        xr_xir_value_drop(&result); CHECK(live == baseline);
    }
    for (uint64_t prefix = full ? 0 : exact - 1; prefix <= exact; ++prefix) {
        admission->work = prefix;
        CHECK(projection_value_op(operation, source, admission, &result) ==
            (prefix == exact ? XR_XIR_VALUE_OK : XR_XIR_VALUE_LIMIT));
        CHECK(source->payload == payload && !memcmp(map->data, saved, bytes));
        xr_xir_value_drop(&result); CHECK(live == baseline);
    }
    printf("projection operation=%u actual allocation sites=%zu exact work=%llu mode=%s root/physical preservation PASS\n",
        operation, points, (unsigned long long)exact, full ? "full" : "baseline");
}
static void projection_value_scaling(XrXirValueAdmission *admission, const XrXirValue *text) {
    uint64_t previous[3] = {0};
    for (uint32_t count = 64; count <= 256; count *= 2) {
        XrXirValue keys[256], values[256], map = {0};
        for (uint32_t i = 0; i < count; ++i) {
            keys[i] = (XrXirValue){XR_XIR_I64, 0, i}; values[i] = *text;
        }
        admission->work = 100000000;
        CHECK(xr_xir_map_new((XrXirType)256, keys, values, count, admission, &map) == XR_XIR_VALUE_OK);
        for (unsigned operation = 0; operation < 3; ++operation) {
            admission->work = 100000000; XrXirValue result = {0};
            CHECK(projection_value_op(operation, &map, admission, &result) == XR_XIR_VALUE_OK);
            uint64_t work = 100000000 - admission->work;
            CHECK(work && (!previous[operation] || work <= previous[operation] * 3));
            previous[operation] = work; xr_xir_value_drop(&result);
            printf("projection linear traversal operation=%u count=%u work=%llu PASS\n", operation, count, (unsigned long long)work);
        }
        xr_xir_value_drop(&map);
    }
}
int main(int argc, char **argv) {
    CHECK(argc == 1 || (argc == 2 && !strcmp(argv[1], "--full")));
    XrXirTypeArena *arena = projection_value_arena(); XrXirDomain *domain = NULL;
    CHECK(xr_xir_domain_new(1048576, &domain) == XR_XIR_VALUE_OK);
    XrXirValueAdmission admission = {arena, domain, NULL, NULL, 1000000, 65536};
    XrXirValue keys[] = {{XR_XIR_I64, 0, 2}, {XR_XIR_I64, 0, 1}}, strings[2] = {{0}}, map = {0}, set = {0};
    CHECK(xr_xir_string_new(domain, "first-owned-string", 18, strings) == XR_XIR_VALUE_OK);
    CHECK(xr_xir_string_new(domain, "second-owned-string", 19, strings + 1) == XR_XIR_VALUE_OK);
    CHECK(xr_xir_map_new((XrXirType)256, keys, strings, 2, &admission, &map) == XR_XIR_VALUE_OK);
    CHECK(xr_xir_set_new((XrXirType)257, strings, 2, &admission, &set) == XR_XIR_VALUE_OK);
    for (unsigned operation = 0; operation < 4; ++operation)
        projection_value_faults(operation, operation == 3 ? &set : &map, &admission, argc == 2);
    projection_value_scaling(&admission, strings);
    XrXirValue output = {0}; admission.work = 1000000;
    CHECK(xr_xir_map_keys(&set, (XrXirType)259, &admission, &output) == XR_XIR_VALUE_BAD_ARGUMENT);
    CHECK(xr_xir_map_keys(&map, (XrXirType)262, &admission, &output) == XR_XIR_VALUE_BAD_ARGUMENT);
    CHECK(xr_xir_map_entries(&map, (XrXirType)259, &admission, &output) == XR_XIR_VALUE_BAD_ARGUMENT);
    CHECK(xr_xir_map_keys(&map, (XrXirType)258, NULL, &output) == XR_XIR_VALUE_BAD_ARGUMENT);
    CHECK(!output.type && !output.reserved && !output.payload);
    output = keys[0];
    CHECK(xr_xir_map_keys(&map, (XrXirType)258, &admission, &output) == XR_XIR_VALUE_BAD_ARGUMENT && output.payload == 2);
    xr_xir_value_drop(&map); xr_xir_value_drop(&set);
    xr_xir_value_drop(strings); xr_xir_value_drop(strings + 1);
    xr_xir_compile_type_arena_drop(arena); xr_xir_domain_drop(domain);
    CHECK(!live && !value_compile_live && !value_compile_bytes);
    puts("Typed projection shape rejection, owned slots, and final physical0 PASS"); return 0;
}
