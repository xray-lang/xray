/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_p09_set_algebra_value.c - Actual binary Set allocation and work limits
 */
#define XR_COMPILE_DOMAIN_ENTRY original_set_algebra_compile_domain_main
#include "test_xir_compile_domain.c"
#undef XR_COMPILE_DOMAIN_ENTRY
static XrXirTypeArena *algebra_value_arena(void) {
    XrXirTypeNode nodes[] = {
        {.kind = XR_XIR_TYPE_SET, .element = XR_XIR_I64},
        {.kind = XR_XIR_TYPE_SET, .element = XR_XIR_F64},
        {.kind = XR_XIR_TYPE_SET, .element = XR_XIR_STRING},
        {.kind = XR_XIR_TYPE_ARRAY, .element = XR_XIR_I64},
        {.kind = XR_XIR_TYPE_ARRAY, .element = XR_XIR_F64},
        {.kind = XR_XIR_TYPE_ARRAY, .element = XR_XIR_STRING},
        {.kind = XR_XIR_TYPE_MAP, .element = XR_XIR_I64, .result = XR_XIR_I64}
    };
    XrXirTypes types = {nodes, 7, NULL, NULL}; XrXirTypeArena *arena = NULL;
    CHECK(value_compile_arena(&types, 65536, value_compile_limits(1048576, 1048576, 100000), &arena) == XR_XIR_VALUE_OK);
    return arena;
}
static XrXirValueStatus algebra_value_operation(unsigned operation, const XrXirValue *left,
    const XrXirValue *right, XrXirValueAdmission *admission, XrXirValue *output) {
    switch (operation) {
    case 0: return xr_xir_set_union(left, right, admission, output);
    case 1: return xr_xir_set_intersection(left, right, admission, output);
    case 2: return xr_xir_set_difference(left, right, admission, output);
    case 3: return xr_xir_set_symmetric_difference(left, right, admission, output);
    default: {
        bool relation = true;
        XrXirValueStatus status = operation == 4 ? xr_xir_set_is_subset(left, right, admission, &relation) :
            xr_xir_set_is_superset(left, right, admission, &relation);
        if (status == XR_XIR_VALUE_OK) *output = (XrXirValue){XR_XIR_BOOL, 0, relation};
        else CHECK(relation);
        return status;
    }
    }
}
static void algebra_value_order(const XrXirValue *result, const int64_t *expected,
    int64_t count, XrXirValueAdmission *admission) {
    XrXirValue array = {0};
    CHECK(xr_xir_hash_values(result, (XrXirType)259, admission, &array) == XR_XIR_VALUE_OK);
    int64_t length = -1;
    CHECK(xr_xir_array_len(&array, admission, &length) == XR_XIR_VALUE_OK && length == count);
    for (int64_t i = 0; i < count; ++i) {
        XrXirValue item = {0}; XrXirFaultDetail fault = {0};
        CHECK(xr_xir_array_get(&array, i, admission, &item, &fault) == XR_XIR_VALUE_OK &&
            item.type == XR_XIR_I64 && item.payload == expected[i]);
    }
    xr_xir_value_drop(&array);
}
static void algebra_value_faults(unsigned operation, const XrXirValue *left,
    const XrXirValue *right, XrXirValueAdmission *admission, bool full) {
    const int64_t expected[4][4] = {{3,1,2,4}, {1,2}, {3}, {3,4}};
    const int64_t lengths[] = {4,2,1,2};
    XrXirValue result = {0}; const uint64_t initial = 1000000;
    admission->work = initial; size_t before = calls, baseline = live;
    CHECK(algebra_value_operation(operation, left, right, admission, &result) == XR_XIR_VALUE_OK);
    size_t points = calls - before; uint64_t exact = initial - admission->work;
    CHECK((operation >= 4 ? points == 0 : points > 0) && exact > 1);
    admission->work = initial;
    if (operation < 4) algebra_value_order(&result, expected[operation], lengths[operation], admission);
    else CHECK(result.type == XR_XIR_BOOL && !result.payload);
    xr_xir_value_drop(&result); CHECK(live == baseline);
    XirHash *a = (XirHash *)object_pointer(left), *b = (XirHash *)object_pointer(right);
    size_t size = 0; CHECK(hash_data_size(a->capacity, a->stride, &size) && size == 192);
    unsigned char saved_a[192], saved_b[192]; memcpy(saved_a, a->data, size); memcpy(saved_b, b->data, size);
    if (full) for (size_t point = 0; point < points; ++point) {
        admission->work = initial; fail_at = calls + point;
        CHECK(algebra_value_operation(operation, left, right, admission, &result) == XR_XIR_VALUE_OOM && calls > fail_at);
        fail_at = SIZE_MAX;
        CHECK(!result.type && !result.reserved && !result.payload && live == baseline);
        CHECK(!memcmp(saved_a, a->data, size) && !memcmp(saved_b, b->data, size));
        admission->work = initial;
        CHECK(algebra_value_operation(operation, left, right, admission, &result) == XR_XIR_VALUE_OK);
        xr_xir_value_drop(&result); CHECK(live == baseline);
    }
    for (uint64_t prefix = full ? 0 : exact - 1; prefix <= exact; ++prefix) {
        admission->work = prefix;
        CHECK(algebra_value_operation(operation, left, right, admission, &result) ==
            (prefix == exact ? XR_XIR_VALUE_OK : XR_XIR_VALUE_LIMIT));
        CHECK(!memcmp(saved_a, a->data, size) && !memcmp(saved_b, b->data, size));
        xr_xir_value_drop(&result); CHECK(live == baseline);
    }
    printf("Set algebra operation=%u actual allocation sites=%zu exact work=%llu mode=%s input/physical preservation PASS\n",
        operation, points, (unsigned long long)exact, full ? "full" : "baseline");
}
static void algebra_value_ieee(XrXirValueAdmission *admission) {
    XrXirValue a[] = {{XR_XIR_F64,0,0}, {XR_XIR_F64,0,INT64_C(0x7ff8000000000000)}};
    XrXirValue b[] = {{XR_XIR_F64,0,INT64_MIN}, {XR_XIR_F64,0,(int64_t)UINT64_C(0xfff0000000000001)}, {XR_XIR_F64,0,INT64_C(0x3ff0000000000000)}};
    XrXirValue left = {0}, right = {0}, result = {0}; int64_t length = -1;
    admission->work = 1000000;
    CHECK(xr_xir_set_new((XrXirType)257, a, 2, admission, &left) == XR_XIR_VALUE_OK);
    CHECK(xr_xir_set_new((XrXirType)257, b, 3, admission, &right) == XR_XIR_VALUE_OK);
    const int64_t lengths[] = {3,2,0,1};
    for (unsigned operation = 0; operation < 4; ++operation) {
        CHECK(algebra_value_operation(operation, &left, &right, admission, &result) == XR_XIR_VALUE_OK);
        CHECK(xr_xir_hash_len(&result, admission, &length) == XR_XIR_VALUE_OK && length == lengths[operation]);
        xr_xir_value_drop(&result);
    }
    bool relation = false;
    CHECK(xr_xir_set_is_subset(&left, &right, admission, &relation) == XR_XIR_VALUE_OK && relation);
    CHECK(xr_xir_set_is_superset(&right, &left, admission, &relation) == XR_XIR_VALUE_OK && relation);
    xr_xir_value_drop(&left); xr_xir_value_drop(&right);
}
static void algebra_value_retention(XrXirValueAdmission *admission) {
    XrXirDomain *producer = NULL;
    CHECK(xr_xir_domain_new(65536, &producer) == XR_XIR_VALUE_OK);
    XrXirValue keys[2] = {{0}}, left = {0}, empty = {0}, result = {0};
    CHECK(xr_xir_string_new(producer, "first-owned-string", 18, keys) == XR_XIR_VALUE_OK);
    CHECK(xr_xir_string_new(producer, "second-owned-string", 19, keys + 1) == XR_XIR_VALUE_OK);
    admission->work = 1000000;
    CHECK(xr_xir_set_new((XrXirType)258, keys, 2, admission, &left) == XR_XIR_VALUE_OK);
    CHECK(xr_xir_set_new((XrXirType)258, NULL, 0, admission, &empty) == XR_XIR_VALUE_OK);
    XirObject *first = object_pointer(keys), *last = object_pointer(keys + 1);
    uint32_t first_refs = atomic_load(&first->references), last_refs = atomic_load(&last->references);
    size_t baseline = live;
    atomic_store(&last->references, UINT32_MAX);
    for (unsigned operation = 0; operation < 4; ++operation) {
        admission->work = 1000000;
        CHECK(algebra_value_operation(operation, &left, operation == 1 ? &left : &empty,
            admission, &result) == XR_XIR_VALUE_REFCOUNT_LIMIT);
        CHECK(!result.type && !result.reserved && !result.payload && live == baseline);
        CHECK(atomic_load(&first->references) == first_refs && atomic_load(&last->references) == UINT32_MAX);
    }
    bool relation = false; admission->work = 1000000; size_t before = calls;
    CHECK(xr_xir_set_is_subset(&left, &left, admission, &relation) == XR_XIR_VALUE_OK && relation && calls == before);
    atomic_store(&last->references, last_refs);
    CHECK(xr_xir_set_union(&left, &empty, admission, &result) == XR_XIR_VALUE_OK);
    xr_xir_value_drop(&left); xr_xir_value_drop(&empty);
    xr_xir_value_drop(keys); xr_xir_value_drop(keys + 1);
    xr_xir_domain_close(producer); xr_xir_domain_drop(producer);
    XrXirValue array = {0};
    CHECK(xr_xir_hash_values(&result, (XrXirType)261, admission, &array) == XR_XIR_VALUE_OK);
    xr_xir_value_drop(&result);
    for (int64_t i = 0; i < 2; ++i) {
        XrXirValue item = {0}; XrXirFaultDetail fault = {0};
        CHECK(xr_xir_array_get(&array, i, admission, &item, &fault) == XR_XIR_VALUE_OK);
        const char *bytes = NULL; size_t length = 0;
        const char *expected = i ? "second-owned-string" : "first-owned-string";
        CHECK(xr_xir_string_view(&item, &bytes, &length) && length == strlen(expected) && !memcmp(bytes, expected, length));
        xr_xir_value_drop(&item);
    }
    xr_xir_value_drop(&array);
    puts("Set partial-retain rollback, non-retaining relation and closed producer lifetime PASS");
}
int main(int argc, char **argv) {
    CHECK(argc == 1 || (argc == 2 && !strcmp(argv[1], "--full")));
    XrXirTypeArena *arena = algebra_value_arena(); XrXirDomain *domain = NULL;
    CHECK(xr_xir_domain_new(1048576, &domain) == XR_XIR_VALUE_OK);
    XrXirValueAdmission admission = {arena,domain,NULL,NULL,1000000,65536};
    XrXirValue a[] = {{XR_XIR_I64,0,3},{XR_XIR_I64,0,1},{XR_XIR_I64,0,2}};
    XrXirValue b[] = {{XR_XIR_I64,0,2},{XR_XIR_I64,0,4},{XR_XIR_I64,0,1}};
    XrXirValue left = {0}, right = {0}, alias = {0}, result = {0};
    CHECK(xr_xir_set_new((XrXirType)256, a, 3, &admission, &left) == XR_XIR_VALUE_OK);
    CHECK(xr_xir_set_new((XrXirType)256, b, 3, &admission, &right) == XR_XIR_VALUE_OK);
    CHECK(xr_xir_value_copy(&left, &alias) == XR_XIR_VALUE_OK);
    for (unsigned operation = 0; operation < 6; ++operation)
        algebra_value_faults(operation, &left, &right, &admission, argc == 2);
    admission.work = 1000000;
    CHECK(xr_xir_set_union(&left, &right, NULL, &result) == XR_XIR_VALUE_BAD_ARGUMENT && !result.type);
    CHECK(xr_xir_set_union(NULL, &right, &admission, &result) == XR_XIR_VALUE_BAD_ARGUMENT && !result.type);
    result = a[0];
    CHECK(xr_xir_set_union(&left, &right, &admission, &result) == XR_XIR_VALUE_BAD_ARGUMENT && result.payload == 3);
    algebra_value_ieee(&admission);
    algebra_value_retention(&admission);
    xr_xir_value_drop(&alias); xr_xir_value_drop(&left); xr_xir_value_drop(&right);
    xr_xir_compile_type_arena_drop(arena); xr_xir_domain_drop(domain);
    CHECK(!live && !value_compile_live && !value_compile_bytes);
    puts("Set algebra, IEEE key relation, exact work and final physical0 PASS"); return 0;
}
