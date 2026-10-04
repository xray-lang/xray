/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_error_value_cases.h - Error views preserve enum authority and leases
 */
#ifndef XIR_ERROR_VALUE_CASES_H
#define XIR_ERROR_VALUE_CASES_H
static void error_deep_value_cases(void) {
    enum { DEPTH = 512 };
    const XrXirNominalVariant variants[] = {{{"End", 3}, 0, 0}, {{"Link", 4}, 0, 1}};
    const XrXirNominalFieldIdentity field = {{"inner", 5}, 0};
    const XrXirNominalIdentity identity = {{"alpha", 5}, {"Chain", 5}, 1, 0, &field, 1,
        XR_XIR_NOMINAL_ENUM, variants, 2, 0};
    const XrXirNominalTable table = {NULL, 1, &identity};
    const XrXirType field_type = XR_XIR_ERROR;
    const XrXirTypeNode nodes[] = {
        {.kind = XR_XIR_TYPE_NOMINAL, .nominal = {0, NULL, 0, &field_type, 1}},
        {.kind = XR_XIR_TYPE_ARRAY, .element = XR_XIR_ERROR}};
    const XrXirTypes types = {nodes, 2, &table, NULL};
    XrXirDomain *domain = NULL; XrXirTypeArena *arena = NULL;
    XrCompileResourceLimits limits = value_compile_limits(65536, 65536, 10000);
    CHECK(!live && xr_xir_domain_new(4194304, &domain) == XR_XIR_VALUE_OK);
    CHECK(value_compile_arena(&types, 100, limits, &arena) == XR_XIR_VALUE_OK);
    XrXirValueAdmission admission = {arena, domain, NULL, NULL, 10000000, 65536};
    XrXirValue concrete = {0}, error = {0}, array = {0};
    CHECK(xr_xir_enum_new((XrXirType)256, 0, NULL, 0, &admission, &concrete) == XR_XIR_VALUE_OK);
    CHECK(xr_xir_error_erase(&concrete, &admission, &error) == XR_XIR_VALUE_OK);
    xr_xir_value_drop(&concrete);
    for (uint32_t i = 0; i < DEPTH; ++i) {
        CHECK(xr_xir_enum_new((XrXirType)256, 1, &error, 1, &admission, &concrete) == XR_XIR_VALUE_OK);
        xr_xir_value_drop(&error);
        CHECK(xr_xir_error_erase(&concrete, &admission, &error) == XR_XIR_VALUE_OK);
        xr_xir_value_drop(&concrete);
    }
    CHECK(DEPTH > xr_xir_compile_type_arena_types(arena)->count);
    CHECK(xr_xir_array_new((XrXirType)257, &error, 1, &admission, &array) == XR_XIR_VALUE_OK);
    xr_xir_value_drop(&error); xr_xir_compile_type_arena_drop(arena); xr_xir_domain_drop(domain);
    size_t baseline = live, begin = calls; uint64_t bytes = xr_xir_domain_stats(domain).live_bytes;
    CHECK(xr_xir_value_admit(&array, (XrXirType)257, &admission) == XR_XIR_VALUE_OK);
    size_t sites = calls - begin; CHECK(sites > 1);
    for (size_t i = 0; i < sites; ++i) {
        admission.work = 10000000; fail_at = calls + i;
        CHECK(xr_xir_value_admit(&array, (XrXirType)257, &admission) == XR_XIR_VALUE_OOM);
        fail_at = SIZE_MAX;
        CHECK(admission.scratch_bytes == 65536 && live == baseline && xr_xir_domain_stats(domain).live_bytes == bytes);
    }
    admission.work = 100;
    CHECK(xr_xir_value_admit(&array, (XrXirType)257, &admission) == XR_XIR_VALUE_LIMIT);
    CHECK(live == baseline && admission.scratch_bytes == 65536);
    admission.work = 10000000;
    XrXirFaultDetail fault = {0};
    CHECK(xr_xir_array_get(&array, 0, &admission, &error, &fault) == XR_XIR_VALUE_OK);
    xr_xir_value_drop(&array);
    for (uint32_t i = 0; i <= DEPTH; ++i) {
        CHECK(xr_xir_error_narrow(&error, (XrXirType)256, &admission, &concrete) == XR_XIR_VALUE_OK);
        xr_xir_value_drop(&error);
        uint32_t variant = 99;
        CHECK(xr_xir_enum_variant(&concrete, &variant) == XR_XIR_VALUE_OK && variant == (i < DEPTH ? 1u : 0u));
        if (i < DEPTH) CHECK(xr_xir_enum_get(&concrete, 1, 0, &admission, &error) == XR_XIR_VALUE_OK);
        xr_xir_value_drop(&concrete);
    }
    CHECK(!live);
}
static void error_nominal_identity_cases(const XrXirTypes *types, const XrXirValue *pair,
    XrXirValueAdmission *admission) {
    size_t baseline = live;
    XrXirNominalIdentity ids[3]; XrXirTypeNode nodes[3];
    const XrXirLiteral names[] = {{"First", 5}, {"Second", 6}, {"Record", 6}};
    for (uint32_t i = 0; i < 3; ++i) {
        ids[i] = types->nominals->identities[0]; ids[i].name = names[i];
        nodes[i] = types->nodes[0]; nodes[i].nominal.declaration = i;
    }
    ids[2].kind = XR_XIR_NOMINAL_STRUCT; ids[2].variants = NULL; ids[2].variant_count = 0;
    XrXirNominalTable table = {NULL, 3, ids}; XrXirTypes local = {nodes, 3, &table, NULL};
    XrCompileResourceLimits limits = value_compile_limits(65536, 65536, 100000);
    XrXirTypeArena *arena = NULL;
    CHECK(value_compile_arena(&local, 1000, limits, &arena) == XR_XIR_VALUE_OK);
    XrXirValueAdmission receiving = *admission; receiving.arena = arena;
    XrXirValue concrete = {0}, error = {0}, output = {0}, record = {0};
    CHECK(xr_xir_enum_new((XrXirType)256, 0, NULL, 0, &receiving, &concrete) == XR_XIR_VALUE_OK);
    CHECK(xr_xir_error_erase(&concrete, &receiving, &error) == XR_XIR_VALUE_OK);
    CHECK(xr_xir_error_narrow(&error, (XrXirType)257, &receiving, &output) == XR_XIR_VALUE_BAD_ARGUMENT && !output.type);
    bool matches = true;
    size_t allocations = calls; uint32_t references = atomic_load(&object_pointer(&error)->references);
    CHECK(xr_xir_error_is(&error, (XrXirType)257, &receiving, &matches) == XR_XIR_VALUE_OK && !matches);
    CHECK(xr_xir_error_is(&error, (XrXirType)256, &receiving, &matches) == XR_XIR_VALUE_OK && matches);
    CHECK(calls == allocations && atomic_load(&object_pointer(&error)->references) == references);
    CHECK(xr_xir_error_is(&error, (XrXirType)258, &receiving, &matches) == XR_XIR_VALUE_BAD_ARGUMENT && !matches);
    CHECK(xr_xir_error_is(&error, (XrXirType)256, admission, &matches) == XR_XIR_VALUE_BAD_ARGUMENT && !matches);
    XrXirValueAdmission limited = receiving; limited.work = 0; matches = true;
    CHECK(xr_xir_error_is(&error, (XrXirType)256, &limited, &matches) == XR_XIR_VALUE_LIMIT && !matches);

    const XirNominalValue *payload = (const XirNominalValue *) object_pointer(pair);
    CHECK(xr_xir_struct_new((XrXirType)258, payload->fields, 2, &receiving, &record) == XR_XIR_VALUE_OK);
    CHECK(xr_xir_error_erase(&record, &receiving, &output) == XR_XIR_VALUE_BAD_ARGUMENT && !output.type);
    XrXirValue forged = {XR_XIR_ERROR, 0, record.payload};
    CHECK(!xr_xir_value_valid(&forged));
    CHECK(xr_xir_value_admit(&forged, XR_XIR_ERROR, &receiving) == XR_XIR_VALUE_BAD_ARGUMENT);
    xr_xir_value_drop(&record); xr_xir_value_drop(&concrete); xr_xir_value_drop(&error);
    xr_xir_compile_type_arena_drop(arena); CHECK(live == baseline);
}
static void error_value_cases(const XrXirTypes *types, const XrXirValue *empty,
    const XrXirValue *pair, XrXirValueAdmission *admission) {
    error_nominal_identity_cases(types, pair, admission);
    size_t baseline = live;
    XrXirValue error = {0}, copy = {0}, narrowed = {0};
    size_t before = calls;
    CHECK(xr_xir_error_erase(empty, admission, &error) == XR_XIR_VALUE_OK);
    CHECK(error.type == XR_XIR_ERROR && error.payload == empty->payload && calls == before);
    XrXirValue borrowed = {0};
    uint32_t retained = atomic_load(&((XrXirTypeArena *) admission->arena)->references);
    CHECK(xr_xir_error_borrow(&error, &borrowed) && borrowed.type == empty->type && borrowed.payload == empty->payload);
    CHECK(atomic_load(&((XrXirTypeArena *) admission->arena)->references) == retained);
    borrowed = (XrXirValue) {0};
    CHECK(!xr_xir_error_borrow(empty, &borrowed) && !borrowed.type);
    CHECK(xr_xir_value_arena(&error) == admission->arena);
    CHECK(xr_xir_value_copy(&error, &copy) == XR_XIR_VALUE_OK);
    CHECK(xr_xir_error_narrow(&copy, (XrXirType)256, admission, &narrowed) == XR_XIR_VALUE_OK);
    CHECK(narrowed.type == 256 && narrowed.payload == empty->payload && calls == before);
    xr_xir_value_drop(&copy); xr_xir_value_drop(&narrowed);
    uint32_t variant = 99;
    CHECK(xr_xir_enum_variant(&error, &variant) == XR_XIR_VALUE_BAD_ARGUMENT && variant == 99);
    CHECK(xr_xir_enum_get(&error, 0, 0, admission, &copy) == XR_XIR_VALUE_BAD_ARGUMENT && !copy.type);
    XrXirValueAdmission limited = *admission; limited.work = 0;
    CHECK(xr_xir_error_narrow(&error, (XrXirType)256, &limited, &copy) == XR_XIR_VALUE_LIMIT && !copy.type);
    CHECK(xr_xir_error_erase(empty, &limited, &copy) == XR_XIR_VALUE_LIMIT && !copy.type);
    CHECK(xr_xir_error_narrow(&error, XR_XIR_I64, admission, &copy) == XR_XIR_VALUE_BAD_ARGUMENT && !copy.type);
    XrXirTypeArena *arena = (XrXirTypeArena *) admission->arena;
    uint32_t leases = atomic_load(&arena->references);
    atomic_store(&arena->references, UINT32_MAX);
    CHECK(xr_xir_error_erase(empty, admission, &copy) == XR_XIR_VALUE_REFCOUNT_LIMIT && !copy.type);
    CHECK(xr_xir_error_narrow(&error, (XrXirType)256, admission, &copy) == XR_XIR_VALUE_REFCOUNT_LIMIT && !copy.type);
    atomic_store(&arena->references, leases);
    XrCompileResourceLimits limits = value_compile_limits(65536, 65536, 10000);
    XrXirTypeArena *foreign = NULL;
    CHECK(value_compile_arena(types, 100, limits, &foreign) == XR_XIR_VALUE_OK);
    XrXirValueAdmission receiving = *admission; receiving.arena = foreign;
    CHECK(!xr_xir_value_argument(&error, foreign, XR_XIR_ERROR));
    CHECK(xr_xir_value_admit(&error, XR_XIR_ERROR, &receiving) == XR_XIR_VALUE_BAD_ARGUMENT);
    CHECK(xr_xir_error_erase(empty, &receiving, &copy) == XR_XIR_VALUE_BAD_ARGUMENT && !copy.type);
    CHECK(xr_xir_error_narrow(&error, (XrXirType)256, &receiving, &copy) == XR_XIR_VALUE_BAD_ARGUMENT && !copy.type);
    xr_xir_compile_type_arena_drop(foreign);
    int64_t slot = 0;
    CHECK(xr_xir_owned_slot_copy(&slot, 0, arena, XR_XIR_ERROR, error.payload) == XR_XIR_VALUE_OK);
    xr_xir_value_drop(&error); xr_xir_owned_slot_clear(&slot, 0); CHECK(!slot);
    before = calls;
    CHECK(xr_xir_error_erase(pair, admission, &error) == XR_XIR_VALUE_OK);
    CHECK(error.payload == pair->payload && calls == before + 1 && live == baseline);
    CHECK(xr_xir_enum_get(&error, 1, 0, admission, &copy) == XR_XIR_VALUE_BAD_ARGUMENT && !copy.type);
    fail_at = calls;
    CHECK(xr_xir_error_narrow(&error, (XrXirType)256, admission, &copy) == XR_XIR_VALUE_OOM && !copy.type);
    fail_at = SIZE_MAX;
    XirObject *object = object_pointer(pair); uint32_t references = atomic_load(&object->references);
    atomic_store(&object->references, UINT32_MAX);
    CHECK(xr_xir_error_narrow(&error, (XrXirType)256, admission, &copy) == XR_XIR_VALUE_REFCOUNT_LIMIT && !copy.type);
    atomic_store(&object->references, references);
    XrXirValue fake = {XR_XIR_ERROR, 0, ((XirNominalValue *)object)->fields[0].payload};
    CHECK(!xr_xir_value_valid(&fake));
    CHECK(xr_xir_error_narrow(&fake, (XrXirType)256, admission, &copy) == XR_XIR_VALUE_BAD_ARGUMENT && !copy.type);
    XirNominalValue *record = (XirNominalValue *) object; uint32_t saved = record->variant;
    record->variant = 99; CHECK(!xr_xir_value_valid(&error)); record->variant = saved;
    CHECK(xr_xir_error_narrow(&error, (XrXirType)256, admission, &copy) == XR_XIR_VALUE_OK);
    CHECK(xr_xir_enum_get(&copy, 1, 0, admission, &narrowed) == XR_XIR_VALUE_OK);
    const char *bytes = NULL; size_t length = 0;
    CHECK(xr_xir_string_view(&narrowed, &bytes, &length) && length == 4 && !memcmp(bytes, "left", 4));
    xr_xir_value_drop(&narrowed); xr_xir_value_drop(&copy); xr_xir_value_drop(&error);
    CHECK(live == baseline && xr_xir_value_valid(empty) && xr_xir_value_valid(pair));
}
#endif // XIR_ERROR_VALUE_CASES_H
