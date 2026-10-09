/*
 * xray - Lightweight typed scripting with native concurrency
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_foreign_array_fi_cases.h - Foreign spare push and owned SET rollback
 */
#ifndef XIR_FOREIGN_ARRAY_FI_CASES_H
#define XIR_FOREIGN_ARRAY_FI_CASES_H
typedef struct ForeignArray {
    XrXirDomain *domains[2];
    XrXirTypeArena *arena;
    XrXirValueAdmission local, remote;
    XrXirValue array, old_text, new_text, function, some, replacement;
    size_t releases;
} ForeignArray;

static void foreign_array_prepare(ForeignArray *f) {
    CHECK(!live && !value_compile_live && !value_compile_bytes && !closed_fail_all);
    *f = (ForeignArray){0}; fail_at = SIZE_MAX;
    const XrXirDomainBudgetControls controls = {65536, 65536, 10000000, 65536, 65536};
    for (unsigned i = 0; i < 2; ++i)
        CHECK(xr_xir_domain_new_budgeted(65536, &controls, &f->domains[i]) == XR_XIR_VALUE_OK);
    const XrXirCallableParameter fields[] = {{(XrXirType)257, 0}, {XR_XIR_STRING, 0}};
    const XrXirTypeNode nodes[] = {
        {.kind = XR_XIR_TYPE_CALLABLE, .result = XR_XIR_I64, .flags = XR_XIR_CALLABLE_ROOT_UNRESOLVED},
        {.kind = XR_XIR_TYPE_NULLABLE, .element = (XrXirType)256},
        {.kind = XR_XIR_TYPE_TUPLE, .parameters = fields, .parameter_count = 2},
        {.kind = XR_XIR_TYPE_ARRAY, .element = (XrXirType)258}};
    const XrXirTypes types = {nodes, 4, NULL, NULL};
    CHECK(value_compile_arena(&types, 65536, value_compile_limits(65536, 1048576, 65536), &f->arena) == XR_XIR_VALUE_OK);
    f->local = allocation_admission(f->domains[0], f->arena, &f->releases);
    f->remote = allocation_admission(f->domains[1], f->arena, &f->releases);
    f->local.scratch_bytes = f->remote.scratch_bytes = 65536;
    XrXirValue none = {0}, tuple = {0};
    CHECK(xr_xir_nullable_new((XrXirType)257, NULL, &f->remote, &none) == XR_XIR_VALUE_OK);
    CHECK(xr_xir_string_new(f->domains[1], "old", 3, &f->old_text) == XR_XIR_VALUE_OK);
    XrXirValue old_fields[] = {none, f->old_text};
    CHECK(xr_xir_tuple_new((XrXirType)258, old_fields, 2, &f->remote, &tuple) == XR_XIR_VALUE_OK);
    CHECK(xr_xir_array_new((XrXirType)259, &tuple, 1, &f->remote, &f->array) == XR_XIR_VALUE_OK);
    xr_xir_value_drop(&tuple); xr_xir_value_drop(&none);
    XirArray *array = (XirArray *)object_pointer(&f->array);
    CHECK(array->object.domain == f->domains[1] && atomic_load(&array->object.references) == 1);
    CHECK(array->length == 1 && array->capacity == 4);
    CHECK(atomic_load(&object_pointer(&f->old_text)->references) == 2);
    XrXirFunctionBinding binding = {&f->releases, capture_release, 0, NULL, 0};
    CHECK(xr_xir_function_new(f->domains[0], f->arena, (XrXirType)256, &binding, &f->local, &f->function) == XR_XIR_VALUE_OK);
    CHECK(xr_xir_nullable_new((XrXirType)257, &f->function, &f->local, &f->some) == XR_XIR_VALUE_OK);
    CHECK(xr_xir_string_new(f->domains[0], "new", 3, &f->new_text) == XR_XIR_VALUE_OK);
    XrXirValue new_fields[] = {f->some, f->new_text};
    CHECK(xr_xir_tuple_new((XrXirType)258, new_fields, 2, &f->local, &f->replacement) == XR_XIR_VALUE_OK);
}

static void foreign_array_observe(ForeignArray *f, bool append, bool changed) {
    /* Failed LIMIT cases are read through B without refilling A's spent ledger. */
    XrXirValueAdmission *read = changed ? &f->local : &f->remote;
    int64_t length = -1; XrXirFaultDetail fault = {0};
    CHECK(xr_xir_array_len(&f->array, read, &length) == XR_XIR_VALUE_OK && length == (changed && append ? 2 : 1));
    for (int64_t i = 0; i < length; ++i) {
        bool replaced = changed && (!append || i == 1);
        XrXirValue tuple = {0}, field = {0};
        CHECK(xr_xir_array_get(&f->array, i, read, &tuple, &fault) == XR_XIR_VALUE_OK && !fault.code);
        CHECK(xr_xir_tuple_get(&tuple, 0, &field) == XR_XIR_VALUE_OK);
        bool some = false; const XrXirValue *payload = NULL;
        CHECK(xr_xir_nullable_view(&field, &some, &payload) && some == replaced);
        if (replaced) CHECK(payload && payload->payload == f->function.payload);
        else CHECK(!payload);
        xr_xir_value_drop(&field);
        CHECK(xr_xir_tuple_get(&tuple, 1, &field) == XR_XIR_VALUE_OK);
        cross_extra_text(&field, replaced ? "new" : "old");
        xr_xir_value_drop(&field); xr_xir_value_drop(&tuple);
    }
    cross_extra_text(&f->old_text, "old"); cross_extra_text(&f->new_text, "new");
}

/* boundary: 0=normal/OOM; 1=work0; 2=scratch0; 3=live headroom0;
 * 4=requested headroom0. These are independent early LIMIT boundaries. */
static size_t foreign_array_attempt(bool append, size_t point, unsigned boundary) {
    ForeignArray f; foreign_array_prepare(&f);
    CHECK(boundary <= 4 && (!boundary || point == SIZE_MAX));
    XrXirValue original = f.array;
    XirArray *backing = (XirArray *)object_pointer(&f.array);
    unsigned char old_slot[sizeof(uint64_t)];
    CHECK(backing->stride == sizeof(old_slot)); memcpy(old_slot, backing->data, sizeof(old_slot));
    XrXirDomainStats a = xr_xir_domain_stats(f.domains[0]), b = xr_xir_domain_stats(f.domains[1]);
    uint64_t requested = xr_xir_domain_budget_stats(f.domains[0]).requested_bytes;
    uint32_t function_refs = atomic_load(&object_pointer(&f.function)->references);
    uint32_t tuple_refs = atomic_load(&object_pointer(&f.replacement)->references);
    uint32_t string_refs = atomic_load(&object_pointer(&f.new_text)->references);
    if (boundary == 1) f.local.work = 0;
    if (boundary == 2) f.local.scratch_bytes = 0;
    if (boundary == 3) f.domains[0]->limit = a.live_bytes;
    if (boundary == 4) f.domains[0]->budget.requested_limit = requested;
    size_t physical = live, compiler = value_compile_live, compiler_bytes = value_compile_bytes, begin = calls;
    if (point != SIZE_MAX) { CHECK(point <= SIZE_MAX - begin); fail_at = begin + point; }
    XrXirValuePlace place = {(XrXirType)259, &f.array.payload}; XrXirFaultDetail fault = {0};
    XrXirValueStatus status = append ? xr_xir_array_push(&place, &f.replacement, &f.local) :
        xr_xir_array_set(&place, 0, &f.replacement, &f.local, &fault);
    size_t attempts = calls - begin; fail_at = SIZE_MAX;
    CHECK(!fault.code && !f.releases);
    bool changed = !boundary && point == SIZE_MAX;
    if (changed) {
        CHECK(status == XR_XIR_VALUE_OK && attempts > 0 && f.array.payload != original.payload);
        CHECK(object_pointer(&f.array)->domain == f.domains[0]);
        CHECK(atomic_load(&object_pointer(&f.old_text)->references) == (append ? 2u : 1u));
        CHECK(atomic_load(&object_pointer(&f.replacement)->references) == tuple_refs + 1);
    } else {
        CHECK(status == (boundary ? XR_XIR_VALUE_LIMIT : XR_XIR_VALUE_OOM));
        CHECK(attempts == (boundary ? 0u : point + 1));
        CHECK(!memcmp(&f.array, &original, sizeof(original)) && backing->length == 1 && backing->capacity == 4);
        CHECK(!memcmp(backing->data, old_slot, sizeof(old_slot)) && atomic_load(&backing->object.references) == 1);
        CHECK(atomic_load(&object_pointer(&f.old_text)->references) == 2);
        CHECK(atomic_load(&object_pointer(&f.function)->references) == function_refs);
        CHECK(atomic_load(&object_pointer(&f.replacement)->references) == tuple_refs);
        CHECK(atomic_load(&object_pointer(&f.new_text)->references) == string_refs);
        CHECK(live == physical && value_compile_live == compiler && value_compile_bytes == compiler_bytes);
        CHECK(xr_xir_domain_stats(f.domains[0]).live_bytes == a.live_bytes && xr_xir_domain_stats(f.domains[1]).live_bytes == b.live_bytes);
        if (boundary) CHECK(xr_xir_domain_budget_stats(f.domains[0]).requested_bytes == requested);
    }
    CHECK(f.local.scratch_bytes == (boundary == 2 ? 0u : 65536u));
    foreign_array_observe(&f, append, changed);
    size_t cleanup; closed_no_alloc_begin(&cleanup);
    xr_xir_compile_type_arena_drop(f.arena);
    xr_xir_value_drop(&f.array); xr_xir_value_drop(&f.replacement); xr_xir_value_drop(&f.some);
    xr_xir_value_drop(&f.function); xr_xir_value_drop(&f.old_text); xr_xir_value_drop(&f.new_text);
    xr_xir_domain_close(f.domains[0]); xr_xir_domain_close(f.domains[1]);
    xr_xir_domain_drop(f.domains[0]); xr_xir_domain_drop(f.domains[1]);
    CHECK(f.releases == 1 && !live && !value_compile_live && !value_compile_bytes); closed_no_alloc_end(cleanup);
    printf("FOREIGN_ARRAY_POINT append=%u boundary=%u point=%zu attempts=%zu status=%u physical=0/0 cleanup=0\n",
        (unsigned)append, boundary, point, attempts, (unsigned)status);
    return attempts;
}

static void foreign_array_fi_cases(void) {
    for (unsigned append = 0; append < 2; ++append) {
        size_t sites = foreign_array_attempt(append != 0, SIZE_MAX, 0);
        printf("FOREIGN_ARRAY_CENSUS append=%u actual_sites=%zu\n", append, sites); fflush(stdout);
        for (size_t point = 0; point < sites; ++point) (void)foreign_array_attempt(append != 0, point, 0);
        for (unsigned boundary = 1; boundary <= 4; ++boundary) (void)foreign_array_attempt(append != 0, SIZE_MAX, boundary);
        printf("FOREIGN_ARRAY_COMPLETE append=%u actual_points=%zu boundaries=4 no_skip=1\n", append, sites); fflush(stdout);
    }
}
#endif // XIR_FOREIGN_ARRAY_FI_CASES_H
