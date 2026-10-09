/*
 * xray - Lightweight typed scripting with native concurrency
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_foreign_write_fi_cases.h - Owner-sensitive struct/path writes and real OOM
 */
#ifndef XIR_FOREIGN_WRITE_FI_CASES_H
#define XIR_FOREIGN_WRITE_FI_CASES_H
typedef struct ForeignWrite {
    XrXirDomain *domains[2];
    XrXirTypeArena *arena;
    XrXirValueAdmission local, remote;
    XrXirValue root, alias, function, replacement;
    size_t releases;
} ForeignWrite;

static XrXirTypeArena *foreign_write_arena(void) {
    const XrXirNominalFieldIdentity inner_fields[] = {
        {{"callback", 8}, XR_XIR_FIELD_MUTABLE}, {{"text", 4}, 0}, {{"number", 6}, 0}};
    const XrXirNominalFieldIdentity outer_fields[] = {{{"items", 5}, XR_XIR_FIELD_MUTABLE}, {{"sibling", 7}, 0}};
    const XrXirNominalIdentity identities[] = {
        {{"test", 4}, {"ForeignInner", 12}, 1, 0, inner_fields, 3, XR_XIR_NOMINAL_STRUCT, NULL, 0, 0, {0}},
        {{"test", 4}, {"ForeignOuter", 12}, 1, 0, outer_fields, 2, XR_XIR_NOMINAL_STRUCT, NULL, 0, 0, {0}}};
    const XrXirNominalTable table = {NULL, 2, identities};
    const XrXirType inner[] = {(XrXirType)257, XR_XIR_STRING, XR_XIR_I64};
    const XrXirType outer[] = {(XrXirType)259, XR_XIR_I64};
    const XrXirTypeNode nodes[] = {
        {.kind = XR_XIR_TYPE_CALLABLE, .result = XR_XIR_I64, .flags = XR_XIR_CALLABLE_ROOT_UNRESOLVED},
        {.kind = XR_XIR_TYPE_NULLABLE, .element = (XrXirType)256},
        {.kind = XR_XIR_TYPE_NOMINAL, .nominal = {0, NULL, 0, inner, 3}},
        {.kind = XR_XIR_TYPE_ARRAY, .element = (XrXirType)258},
        {.kind = XR_XIR_TYPE_NOMINAL, .nominal = {1, NULL, 0, outer, 2}},
        {.kind = XR_XIR_TYPE_ARRAY, .element = (XrXirType)260}};
    const XrXirTypes types = {nodes, 6, &table, NULL}; XrXirTypeArena *arena = NULL;
    CHECK(value_compile_arena(&types, 65536, value_compile_limits(65536, 1048576, 65536), &arena) == XR_XIR_VALUE_OK);
    return arena;
}

static void foreign_write_prepare(ForeignWrite *f, unsigned mode, bool alias) {
    CHECK(mode < 3 && !live && !value_compile_live && !value_compile_bytes);
    *f = (ForeignWrite){0}; fail_at = SIZE_MAX;
    for (unsigned i = 0; i < 2; ++i) CHECK(xr_xir_domain_new(65536, &f->domains[i]) == XR_XIR_VALUE_OK);
    f->arena = foreign_write_arena();
    f->local = allocation_admission(f->domains[0], f->arena, &f->releases);
    f->remote = allocation_admission(f->domains[1], f->arena, &f->releases);
    f->local.scratch_bytes = f->remote.scratch_bytes = 65536;
    XrXirValue none = {0}, text = {0}, inner = {0}, array = {0}, outer = {0};
    CHECK(xr_xir_nullable_new((XrXirType)257, NULL, &f->remote, &none) == XR_XIR_VALUE_OK);
    CHECK(xr_xir_string_new(f->domains[1], "old", 3, &text) == XR_XIR_VALUE_OK);
    XrXirValue fields[] = {none, text, {XR_XIR_I64, 0, 11}};
    CHECK(xr_xir_struct_new((XrXirType)258, fields, 3, &f->remote, &inner) == XR_XIR_VALUE_OK);
    xr_xir_value_drop(&none); xr_xir_value_drop(&text);
    if (!mode) { f->root = inner; inner = (XrXirValue){0}; }
    else {
        CHECK(xr_xir_array_new((XrXirType)259, &inner, 1, &f->remote, &array) == XR_XIR_VALUE_OK);
        xr_xir_value_drop(&inner);
        XrXirValue outer_values[] = {array, {XR_XIR_I64, 0, 37}};
        CHECK(xr_xir_struct_new((XrXirType)260, outer_values, 2, &f->remote, &outer) == XR_XIR_VALUE_OK);
        xr_xir_value_drop(&array);
        if (mode == 1) { f->root = outer; outer = (XrXirValue){0}; }
        else {
            CHECK(xr_xir_array_new((XrXirType)261, &outer, 1, &f->remote, &f->root) == XR_XIR_VALUE_OK);
            xr_xir_value_drop(&outer);
        }
    }
    if (alias) CHECK(xr_xir_value_copy(&f->root, &f->alias) == XR_XIR_VALUE_OK);
    XrXirFunctionBinding binding = {&f->releases, capture_release, 0, NULL, 0};
    CHECK(xr_xir_function_new(f->domains[0], f->arena, (XrXirType)256, &binding, &f->local, &f->function) == XR_XIR_VALUE_OK);
    CHECK(xr_xir_nullable_new((XrXirType)257, &f->function, &f->local, &f->replacement) == XR_XIR_VALUE_OK);
    CHECK(object_pointer(&f->root)->domain == f->domains[1]);
}

static const XrXirValuePathStep foreign_write_steps[] = {
    {XR_XIR_PATH_INDEX, (XrXirType)261, 0}, {XR_XIR_PATH_FIELD, (XrXirType)260, 0},
    {XR_XIR_PATH_INDEX, (XrXirType)259, 0}, {XR_XIR_PATH_FIELD, (XrXirType)258, 0}};

static XrXirValueStatus foreign_write_apply(ForeignWrite *f, unsigned mode) {
    XrXirValuePlace place = {(XrXirType)f->root.type, &f->root.payload};
    if (!mode) return xr_xir_struct_set(&place, 0, &f->replacement, &f->local);
    XrXirValuePath path = {foreign_write_steps + (mode == 1 ? 1 : 0), mode == 1 ? 3u : 4u};
    XrXirFaultDetail fault = {0};
    XrXirValueStatus status = xr_xir_value_path_write(&place, &path, &f->replacement, &f->local, &fault);
    CHECK(!fault.code); return status;
}

/* Observe through actual getters, after FI is disabled. Getter allocations are
 * outside the mutation census and every temporary owner is dropped here. */
static void foreign_write_observe(ForeignWrite *f, const XrXirValue *root, unsigned mode, bool changed) {
    XrXirValue inner = {0}, outer = {0}, array = {0}, field = {0}; XrXirFaultDetail fault = {0};
    CHECK(object_pointer(root)->domain == f->domains[changed ? 0 : 1]);
    if (!mode) CHECK(xr_xir_value_copy(root, &inner) == XR_XIR_VALUE_OK);
    else {
        if (mode == 1) CHECK(xr_xir_value_copy(root, &outer) == XR_XIR_VALUE_OK);
        else CHECK(xr_xir_array_get(root, 0, &f->local, &outer, &fault) == XR_XIR_VALUE_OK);
        CHECK(xr_xir_struct_get(&outer, 1, &f->local, &field) == XR_XIR_VALUE_OK && field.type == XR_XIR_I64 && field.payload == 37);
        xr_xir_value_drop(&field);
        CHECK(xr_xir_struct_get(&outer, 0, &f->local, &array) == XR_XIR_VALUE_OK);
        CHECK(object_pointer(&array)->domain == f->domains[changed ? 0 : 1]);
        int64_t length = -1;
        CHECK(xr_xir_array_len(&array, &f->local, &length) == XR_XIR_VALUE_OK && length == 1);
        CHECK(xr_xir_array_get(&array, 0, &f->local, &inner, &fault) == XR_XIR_VALUE_OK);
    }
    CHECK(xr_xir_struct_get(&inner, 0, &f->local, &field) == XR_XIR_VALUE_OK);
    bool some = false; const XrXirValue *payload = NULL;
    CHECK(xr_xir_nullable_view(&field, &some, &payload) && some == changed);
    if (changed) {
        CHECK(payload && payload->payload == f->function.payload);
        const XrXirFunctionBinding *binding = xr_xir_function_binding(payload);
        CHECK(binding && binding->owner == &f->releases && binding->release == capture_release && binding->entry == 0 && !binding->capture_count);
    } else CHECK(!payload);
    xr_xir_value_drop(&field);
    CHECK(xr_xir_struct_get(&inner, 1, &f->local, &field) == XR_XIR_VALUE_OK);
    const char *bytes = NULL; size_t length = 0;
    CHECK(xr_xir_string_view(&field, &bytes, &length) && length == 3 && !memcmp(bytes, "old", 3));
    xr_xir_value_drop(&field);
    CHECK(xr_xir_struct_get(&inner, 2, &f->local, &field) == XR_XIR_VALUE_OK && field.type == XR_XIR_I64 && field.payload == 11);
    xr_xir_value_drop(&field); xr_xir_value_drop(&inner); xr_xir_value_drop(&array); xr_xir_value_drop(&outer);
}

static size_t foreign_write_attempt(unsigned mode, bool alias, size_t point) {
    ForeignWrite f; foreign_write_prepare(&f, mode, alias);
    XrXirValue before_value = f.root; size_t before_live = live, begin = calls;
    size_t compiler_blocks = value_compile_live, compiler_bytes = value_compile_bytes;
    XrXirDomainStats a = xr_xir_domain_stats(f.domains[0]), b = xr_xir_domain_stats(f.domains[1]);
    if (point != SIZE_MAX) { CHECK(point <= SIZE_MAX - begin); fail_at = begin + point; }
    XrXirValueStatus status = foreign_write_apply(&f, mode);
    size_t attempts = calls - begin; fail_at = SIZE_MAX;
    if (point == SIZE_MAX) {
        CHECK(status == XR_XIR_VALUE_OK && attempts > 0);
        CHECK(f.root.payload != before_value.payload);
        foreign_write_observe(&f, &f.root, mode, true);
    } else {
        CHECK(status == XR_XIR_VALUE_OOM && attempts == point + 1);
        CHECK(!memcmp(&f.root, &before_value, sizeof(f.root)) && live == before_live);
        CHECK(xr_xir_domain_stats(f.domains[0]).live_bytes == a.live_bytes &&
            xr_xir_domain_stats(f.domains[1]).live_bytes == b.live_bytes);
        CHECK(value_compile_live == compiler_blocks && value_compile_bytes == compiler_bytes);
        CHECK(f.local.scratch_bytes == 65536 && !f.releases);
        foreign_write_observe(&f, &f.root, mode, false);
    }
    if (alias) foreign_write_observe(&f, &f.alias, mode, false);
    size_t cleanup; closed_no_alloc_begin(&cleanup);
    xr_xir_compile_type_arena_drop(f.arena);
    xr_xir_value_drop(&f.alias); xr_xir_value_drop(&f.root); xr_xir_value_drop(&f.replacement); xr_xir_value_drop(&f.function);
    xr_xir_domain_close(f.domains[0]); xr_xir_domain_close(f.domains[1]);
    xr_xir_domain_drop(f.domains[0]); xr_xir_domain_drop(f.domains[1]);
    CHECK(f.releases == 1 && !live && !value_compile_live && !value_compile_bytes);
    closed_no_alloc_end(cleanup);
    printf("FOREIGN_WRITE_FI mode=%u alias=%u point=%zu attempts=%zu status=%u physical=0/0 cleanup_allocations=0\n",
        mode, (unsigned)alias, point, attempts, (unsigned)status);
    return attempts;
}

static void foreign_write_fi_cases(void) {
    for (unsigned mode = 0; mode < 3; ++mode) for (unsigned alias = 0; alias < 2; ++alias) {
        size_t sites = foreign_write_attempt(mode, alias != 0, SIZE_MAX);
        printf("FOREIGN_WRITE_CENSUS mode=%u alias=%u actual_sites=%zu\n", mode, alias, sites); fflush(stdout);
        for (size_t point = 0; point < sites; ++point) (void)foreign_write_attempt(mode, alias != 0, point);
        printf("FOREIGN_WRITE_COMPLETE mode=%u alias=%u actual_points=%zu no_skip=1\n", mode, alias, sites); fflush(stdout);
    }
}
#endif // XIR_FOREIGN_WRITE_FI_CASES_H
