/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_cell_allocation_cases.h - Shared identity, rollback and strong-cycle limits
 *
 * KEY CONCEPT:
 *   A retained cycle is measured as retained storage, never claimed reclaimed.
 */
#ifndef XR_XIR_CELL_ALLOCATION_CASES_H
#define XR_XIR_CELL_ALLOCATION_CASES_H
static void cell_allocation_cases(void) {
    XrXirDomain *domain = NULL, *other = NULL;
    CHECK(xr_xir_domain_new(65536, &domain) == XR_XIR_VALUE_OK);
    CHECK(xr_xir_domain_new(65536, &other) == XR_XIR_VALUE_OK);
    XrXirTypeArena *arena = allocation_arena(domain);
    XrXirValueAdmission admission = allocation_admission(domain, arena, NULL);
    XrXirValue initial = {0}, cell = {0}, alias = {0}, snapshot = {0}, replacement = {0};
    CHECK(xr_xir_string_new(domain, "old", 3, &initial) == XR_XIR_VALUE_OK);
    size_t baseline = live; uint64_t bytes = xr_xir_domain_stats(domain).live_bytes;
    fail_at = calls;
    CHECK(xr_xir_cell_new(domain, arena, (XrXirType) 257, &initial, &admission, &cell) == XR_XIR_VALUE_OOM && !cell.type);
    CHECK(live == baseline && xr_xir_domain_stats(domain).live_bytes == bytes);
    fail_at = SIZE_MAX;
    atomic_store(&object_pointer(&initial)->references, UINT32_MAX);
    CHECK(xr_xir_cell_new(domain, arena, (XrXirType) 257, &initial, &admission, &cell) == XR_XIR_VALUE_REFCOUNT_LIMIT && !cell.type);
    CHECK(live == baseline && xr_xir_domain_stats(domain).live_bytes == bytes);
    atomic_store(&object_pointer(&initial)->references, 1);
    CHECK(xr_xir_cell_new(domain, arena, (XrXirType) 257, &initial, &admission, &cell) == XR_XIR_VALUE_OK);
    CHECK(xr_xir_cell_in_domain(&cell, domain) && !xr_xir_cell_in_domain(&cell, other));
    CHECK(xr_xir_value_copy(&cell, &alias) == XR_XIR_VALUE_OK && alias.payload == cell.payload);
    atomic_store(&object_pointer(&initial)->references, UINT32_MAX);
    CHECK(xr_xir_cell_read(&alias, &snapshot) == XR_XIR_VALUE_REFCOUNT_LIMIT && !snapshot.type);
    atomic_store(&object_pointer(&initial)->references, 2);
    CHECK(xr_xir_cell_read(&alias, &snapshot) == XR_XIR_VALUE_OK);
    CHECK(xr_xir_string_new(domain, "new", 3, &replacement) == XR_XIR_VALUE_OK);
    atomic_store(&object_pointer(&replacement)->references, UINT32_MAX);
    CHECK(xr_xir_cell_write(&alias, &replacement, &admission) == XR_XIR_VALUE_REFCOUNT_LIMIT);
    CHECK(((XirCell *) object_pointer(&cell))->value.payload == initial.payload);
    atomic_store(&object_pointer(&replacement)->references, 1);
    size_t allocations = calls; fail_at = calls;
    CHECK(xr_xir_cell_write(&alias, &replacement, &admission) == XR_XIR_VALUE_OK && calls == allocations);
    CHECK(xr_xir_cell_write(&cell, &cell, &admission) == XR_XIR_VALUE_BAD_ARGUMENT);
    CHECK(xr_xir_cell_write(&cell, &((XirCell *) object_pointer(&cell))->value, &admission) == XR_XIR_VALUE_OK);
    const char *text = NULL; size_t length = 0;
    CHECK(xr_xir_string_view(&snapshot, &text, &length) && length == 3 && !memcmp(text, "old", 3));
    XrXirValue latest = {0}; CHECK(xr_xir_cell_read(&cell, &latest) == XR_XIR_VALUE_OK);
    CHECK(xr_xir_string_view(&latest, &text, &length) && length == 3 && !memcmp(text, "new", 3));
    xr_xir_compile_type_arena_drop(arena); xr_xir_domain_drop(domain); xr_xir_domain_drop(other);
    xr_xir_value_drop(&cell); xr_xir_value_drop(&alias); xr_xir_value_drop(&initial);
    xr_xir_value_drop(&replacement); xr_xir_value_drop(&snapshot); xr_xir_value_drop(&latest);
    CHECK(!live); fail_at = SIZE_MAX;
    CHECK(xr_xir_domain_new(65536, &domain) == XR_XIR_VALUE_OK);
    arena = allocation_arena(domain);
    admission = allocation_admission(domain, arena, NULL);
    domain->limit = xr_xir_domain_stats(domain).live_bytes + sizeof(XirCell) - 1;
    XrXirValue number = {XR_XIR_I64, 0, 1};
    CHECK(xr_xir_cell_new(domain, arena, (XrXirType) 259, &number, &admission, &cell) == XR_XIR_VALUE_LIMIT && !cell.type);
    xr_xir_compile_type_arena_drop(arena); xr_xir_domain_drop(domain); CHECK(!live);
}
static void cell_cycles_and_domains(void) {
    XrXirDomain *domain = NULL, *other = NULL;
    CHECK(xr_xir_domain_new(65536, &domain) == XR_XIR_VALUE_OK);
    CHECK(xr_xir_domain_new(65536, &other) == XR_XIR_VALUE_OK);
    size_t releases = 0;
    XrXirTypeArena *arena = allocation_arena(domain);
    XrXirValueAdmission admission = allocation_admission(domain, arena, &releases);
    XrXirFunctionBinding binding = {&releases, capture_release, 0, NULL, 0};
    XrXirValue empty = {0}, foreign = {0}, cell = {0}, closure = {0}, rejected = {0};
    CHECK(xr_xir_function_new(domain, arena, (XrXirType) 256, &binding, &admission, &empty) == XR_XIR_VALUE_OK);
    XrXirValueAdmission other_admission = allocation_admission(other, arena, &releases);
    CHECK(xr_xir_function_new(other, arena, (XrXirType) 256, &binding, &other_admission, &foreign) == XR_XIR_VALUE_OK);
    CHECK(xr_xir_cell_new(domain, arena, (XrXirType) 258, &foreign, &admission, &cell) == XR_XIR_VALUE_BAD_ARGUMENT);
    CHECK(xr_xir_cell_new(domain, arena, (XrXirType) 258, &empty, &admission, &cell) == XR_XIR_VALUE_OK);
    CHECK(xr_xir_cell_write(&cell, &foreign, &admission) == XR_XIR_VALUE_BAD_ARGUMENT);
    binding.captures = &cell; binding.capture_count = 1;
    CHECK(xr_xir_function_new(other, arena, (XrXirType) 256, &binding, &other_admission, &rejected) == XR_XIR_VALUE_BAD_ARGUMENT);
    CHECK(!releases && !rejected.type);
    CHECK(xr_xir_function_new(domain, arena, (XrXirType) 256, &binding, &admission, &closure) == XR_XIR_VALUE_OK);
    CHECK(xr_xir_cell_write(&cell, &closure, &admission) == XR_XIR_VALUE_OK);
    XrXirValue observed = cell; /* Diagnostic-only borrowed handle, not an RC root. */
    xr_xir_value_drop(&cell); xr_xir_value_drop(&closure);
    CHECK(atomic_load(&object_pointer(&observed)->references) == 1 && releases == 0);
    size_t retained = live, allocations = calls; fail_at = calls;
    CHECK(xr_xir_cell_write(&observed, &empty, &admission) == XR_XIR_VALUE_OK);
    CHECK(live + 2 == retained && calls == allocations && releases == 1);
    observed = (XrXirValue) {0};
    xr_xir_value_drop(&empty); xr_xir_value_drop(&foreign);
    xr_xir_compile_type_arena_drop(arena); xr_xir_domain_drop(domain); xr_xir_domain_drop(other);
    CHECK(releases == 3 && !live); fail_at = SIZE_MAX;
    puts("Shared cells: aliases, copy-before-write rollback, domain rejection and measured strong-cycle retention passed");
}
static void deep_cell_release(void) {
    XrXirDomain *domain = NULL;
    CHECK(xr_xir_domain_new(64u*1024u*1024u, &domain) == XR_XIR_VALUE_OK);
    size_t releases = 0;
    XrXirTypeArena *arena = allocation_arena(domain);
    XrXirValueAdmission admission = allocation_admission(domain, arena, &releases);
    XrXirFunctionBinding binding = {&releases, capture_release, 0, NULL, 0};
    XrXirValue previous = {0};
    CHECK(xr_xir_function_new(domain, arena, (XrXirType) 256, &binding, &admission, &previous) == XR_XIR_VALUE_OK);
    for (uint32_t i = 0; i < 100000; ++i) {
        XrXirValue cell = {0}, next = {0};
        CHECK(xr_xir_cell_new(domain, arena, (XrXirType) 258, &previous, &admission, &cell) == XR_XIR_VALUE_OK);
        binding.captures = &cell; binding.capture_count = 1;
        CHECK(xr_xir_function_new(domain, arena, (XrXirType) 256, &binding, &admission, &next) == XR_XIR_VALUE_OK);
        xr_xir_value_drop(&previous); xr_xir_value_drop(&cell); previous = next;
    }
    xr_xir_compile_type_arena_drop(arena); xr_xir_domain_drop(domain); size_t allocations = calls; fail_at = calls;
    xr_xir_value_drop(&previous);
    CHECK(releases == 100001 && !live && calls == allocations); fail_at = SIZE_MAX;
    puts("Shared cell cleanup: 100000 alternating environments/cells, zero cleanup allocations and zero live blocks");
}
#endif
