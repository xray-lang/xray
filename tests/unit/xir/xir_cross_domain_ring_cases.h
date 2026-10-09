/*
 * xray - Lightweight typed scripting with native concurrency
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_cross_domain_ring_cases.h - Foreign backing at owner-end boundaries
 */
#ifndef XIR_CROSS_DOMAIN_RING_CASES_H
#define XIR_CROSS_DOMAIN_RING_CASES_H

static XrXirTypeArena *cross_ring_arena(void) {
    const XrXirTypeNode nodes[] = {
        {.kind = XR_XIR_TYPE_CALLABLE, .result = XR_XIR_I64, .flags = XR_XIR_CALLABLE_ROOT_UNRESOLVED},
        {.kind = XR_XIR_TYPE_ARRAY, .element = (XrXirType)256},
        {.kind = XR_XIR_TYPE_CELL, .element = (XrXirType)257}};
    const XrXirTypes types = {nodes, 3, NULL, NULL};
    XrXirTypeArena *arena = NULL;
    CHECK(value_compile_arena(&types, 65536, value_compile_limits(65536, 1048576, 65536), &arena) == XR_XIR_VALUE_OK);
    return arena;
}

/* Every identity-bearing write moves a foreign copyable backing to its
 * admission owner, including unique storage with spare capacity. Pure String
 * writes retain their independently tested zero-allocation in-place path. */
static void cross_domain_ring_case(unsigned mode, unsigned order) {
    static const char *const names[] = {"grow", "alias", "spare"};
    CHECK(mode < 3 && order < 2 && !live && !value_compile_live && !value_compile_bytes);
    fail_at = SIZE_MAX;
    XrXirDomain *domains[2] = {NULL, NULL};
    for (unsigned i = 0; i < 2; ++i)
        CHECK(xr_xir_domain_new(65536, &domains[i]) == XR_XIR_VALUE_OK);
    XrXirTypeArena *arena = cross_ring_arena();
    size_t releases = 0;
    XrXirValueAdmission local = allocation_admission(domains[0], arena, &releases);
    XrXirValueAdmission remote = allocation_admission(domains[1], arena, &releases);
    /* Same finite scratch cap used by existing aggregate ownership fixtures. */
    local.scratch_bytes = remote.scratch_bytes = 65536;
    XrXirValue empty = {0}, cell = {0}, function = {0}, array = {0}, old_alias = {0};
    CHECK(xr_xir_array_new((XrXirType)257, NULL, 0, &local, &empty) == XR_XIR_VALUE_OK);
    CHECK(xr_xir_cell_new(domains[0], arena, (XrXirType)258, &empty, &local, &cell) == XR_XIR_VALUE_OK);
    xr_xir_value_drop(&empty);
    XrXirFunctionBinding binding = {&releases, capture_release, 0, &cell, 1};
    CHECK(xr_xir_function_new(domains[0], arena, (XrXirType)256, &binding, &local, &function) == XR_XIR_VALUE_OK);
    CHECK(xr_xir_array_with_capacity((XrXirType)257, mode ? 1 : 0, &remote, &array) == XR_XIR_VALUE_OK);
    CHECK(object_pointer(&cell)->domain == domains[0] && object_pointer(&function)->domain == domains[0]);
    CHECK(object_pointer(&array)->domain == domains[1]);
    int64_t before_payload = array.payload;
    if (mode == 1) CHECK(xr_xir_value_copy(&array, &old_alias) == XR_XIR_VALUE_OK);
    XrXirValuePlace place = {(XrXirType)257, &array.payload};
    CHECK(xr_xir_array_push(&place, &function, &local) == XR_XIR_VALUE_OK);
    CHECK(array.payload != before_payload && object_pointer(&array)->domain == domains[0]);
    int64_t length = -1;
    CHECK(xr_xir_array_len(&array, &local, &length) == XR_XIR_VALUE_OK && length == 1);
    if (mode == 1) {
        CHECK(old_alias.payload == before_payload && object_pointer(&old_alias)->domain == domains[1]);
        CHECK(xr_xir_array_len(&old_alias, &remote, &length) == XR_XIR_VALUE_OK && length == 0);
    }
    XrXirValue observed = {0}; XrXirFaultDetail fault = {0};
    CHECK(xr_xir_array_get(&array, 0, &local, &observed, &fault) == XR_XIR_VALUE_OK);
    const XrXirFunctionBinding *owned = xr_xir_function_binding(&observed);
    CHECK(owned && owned->owner == &releases && owned->release == capture_release && owned->entry == 0 &&
        owned->capture_count == 1 && owned->captures[0].payload == cell.payload);
    xr_xir_value_drop(&observed);
    CHECK(xr_xir_cell_write(&cell, &array, &local) == XR_XIR_VALUE_OK);
    CHECK(xr_xir_cell_read(&cell, &observed) == XR_XIR_VALUE_OK && observed.payload == array.payload);
    xr_xir_value_drop(&observed);
    printf("CROSS_DOMAIN_RING mode=%s order=%s prepared=1 A=Cell,Function array_owner=%s host_roots=0(next)\n",
        names[mode], order ? "B-A" : "A-B", "A");
    fflush(stdout);
    size_t before;
    closed_no_alloc_begin(&before);
    xr_xir_value_drop(&old_alias); xr_xir_value_drop(&array);
    xr_xir_value_drop(&function); xr_xir_value_drop(&cell);
    CHECK(!releases); /* OPEN strong cycles have no ordinary RC-zero event. */
    xr_xir_compile_type_arena_drop(arena); arena = NULL;
    xr_xir_domain_close(domains[order]); xr_xir_domain_close(domains[1u - order]);
    xr_xir_domain_drop(domains[0]); xr_xir_domain_drop(domains[1]);
    printf("CROSS_DOMAIN_RING mode=%s order=%s releases=%zu physical_blocks=%zu compiler_blocks=%zu compiler_bytes=%zu\n",
        names[mode], order ? "B-A" : "A-B", releases, live, value_compile_live, value_compile_bytes);
    fflush(stdout);
    CHECK(releases == 1 && !live && !value_compile_live && !value_compile_bytes);
    closed_no_alloc_end(before);
}

static void cross_domain_ring_cases(void) {
    for (unsigned mode = 0; mode < 3; ++mode)
        for (unsigned order = 0; order < 2; ++order) cross_domain_ring_case(mode, order);
}

#endif // XIR_CROSS_DOMAIN_RING_CASES_H
