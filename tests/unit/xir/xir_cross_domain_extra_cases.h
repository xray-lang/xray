/*
 * xray - Lightweight typed scripting with native concurrency
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_cross_domain_extra_cases.h - Nested foreign paths and owned SET slots
 */
#ifndef XIR_CROSS_DOMAIN_EXTRA_CASES_H
#define XIR_CROSS_DOMAIN_EXTRA_CASES_H
typedef struct CrossExtra {
    XrXirDomain *domains[2];
    XrXirTypeArena *arena;
    XrXirValueAdmission local, remote;
    XrXirValue cell, function;
    size_t releases;
} CrossExtra;

static void cross_extra_begin(CrossExtra *f, const XrXirTypes *types, XrXirType array_type, XrXirType cell_type) {
    CHECK(!live && !value_compile_live && !value_compile_bytes && !closed_fail_all);
    *f = (CrossExtra){0}; fail_at = SIZE_MAX;
    for (unsigned i = 0; i < 2; ++i) CHECK(xr_xir_domain_new(65536, &f->domains[i]) == XR_XIR_VALUE_OK);
    CHECK(value_compile_arena(types, 65536, value_compile_limits(65536, 1048576, 65536), &f->arena) == XR_XIR_VALUE_OK);
    f->local = allocation_admission(f->domains[0], f->arena, &f->releases);
    f->remote = allocation_admission(f->domains[1], f->arena, &f->releases);
    f->local.scratch_bytes = f->remote.scratch_bytes = 65536;
    XrXirValue empty = {0};
    CHECK(xr_xir_array_new(array_type, NULL, 0, &f->local, &empty) == XR_XIR_VALUE_OK);
    CHECK(xr_xir_cell_new(f->domains[0], f->arena, cell_type, &empty, &f->local, &f->cell) == XR_XIR_VALUE_OK);
    xr_xir_value_drop(&empty);
    XrXirFunctionBinding binding = {&f->releases, capture_release, 0, &f->cell, 1};
    CHECK(xr_xir_function_new(f->domains[0], f->arena, (XrXirType)256, &binding, &f->local, &f->function) == XR_XIR_VALUE_OK);
}

static void cross_extra_some(const XrXirValue *value, const CrossExtra *f) {
    bool some = false; const XrXirValue *function = NULL;
    CHECK(xr_xir_nullable_view(value, &some, &function) && some && function && function->payload == f->function.payload);
    const XrXirFunctionBinding *binding = xr_xir_function_binding(function);
    CHECK(binding && binding->owner == &f->releases && binding->release == capture_release && binding->entry == 0 &&
        binding->capture_count == 1 && binding->captures[0].payload == f->cell.payload);
}

static void cross_extra_text(const XrXirValue *value, const char *expected) {
    const char *bytes = NULL; size_t length = 0;
    CHECK(xr_xir_string_view(value, &bytes, &length) && length == 3 && !memcmp(bytes, expected, 3));
}

/* Caller has already dropped every temporary/array/field host owner. */
static void cross_extra_finish(CrossExtra *f, const char *mode, unsigned order) {
    CHECK(order < 2);
    size_t before; closed_no_alloc_begin(&before);
    xr_xir_value_drop(&f->function); xr_xir_value_drop(&f->cell);
    CHECK(!f->releases);
    xr_xir_compile_type_arena_drop(f->arena); f->arena = NULL;
    xr_xir_domain_close(f->domains[order]); xr_xir_domain_close(f->domains[1u - order]);
    xr_xir_domain_drop(f->domains[0]); xr_xir_domain_drop(f->domains[1]);
    printf("CROSS_DOMAIN_EXTRA mode=%s order=%s releases=%zu physical_blocks=%zu compiler_blocks=%zu compiler_bytes=%zu\n",
        mode, order ? "B-A" : "A-B", f->releases, live, value_compile_live, value_compile_bytes);
    fflush(stdout);
    CHECK(f->releases == 1 && !live && !value_compile_live && !value_compile_bytes);
    closed_no_alloc_end(before);
}

static void cross_domain_nested_case(unsigned order) {
    const XrXirTypeNode nodes[] = {
        {.kind = XR_XIR_TYPE_CALLABLE, .result = XR_XIR_I64, .flags = XR_XIR_CALLABLE_ROOT_UNRESOLVED},
        {.kind = XR_XIR_TYPE_NULLABLE, .element = (XrXirType)256},
        {.kind = XR_XIR_TYPE_ARRAY, .element = (XrXirType)257},
        {.kind = XR_XIR_TYPE_CELL, .element = (XrXirType)258},
        {.kind = XR_XIR_TYPE_ARRAY, .element = (XrXirType)258}};
    const XrXirTypes types = {nodes, 5, NULL, NULL}; CrossExtra f;
    cross_extra_begin(&f, &types, (XrXirType)258, (XrXirType)259);
    XrXirValue none = {0}, inner = {0}, outer = {0}, some = {0}, item = {0};
    CHECK(xr_xir_nullable_new((XrXirType)257, NULL, &f.remote, &none) == XR_XIR_VALUE_OK);
    CHECK(xr_xir_array_new((XrXirType)258, &none, 1, &f.remote, &inner) == XR_XIR_VALUE_OK);
    CHECK(xr_xir_array_new((XrXirType)260, &inner, 1, &f.remote, &outer) == XR_XIR_VALUE_OK);
    int64_t inner_payload = inner.payload, outer_payload = outer.payload;
    CHECK(object_pointer(&inner)->domain == f.domains[1] && object_pointer(&outer)->domain == f.domains[1]);
    xr_xir_value_drop(&inner); xr_xir_value_drop(&none);
    CHECK(xr_xir_nullable_new((XrXirType)257, &f.function, &f.local, &some) == XR_XIR_VALUE_OK);
    const XrXirValuePathStep steps[] = {{XR_XIR_PATH_INDEX, (XrXirType)260, 0}, {XR_XIR_PATH_INDEX, (XrXirType)258, 0}};
    const XrXirValuePath path = {steps, 2}; XrXirValuePlace place = {(XrXirType)260, &outer.payload};
    XrXirFaultDetail fault = {0};
    CHECK(xr_xir_value_path_write(&place, &path, &some, &f.local, &fault) == XR_XIR_VALUE_OK);
    /* The new identity edge requires every foreign copyable ancestor to move. */
    CHECK(outer.payload != outer_payload && object_pointer(&outer)->domain == f.domains[0]);
    CHECK(xr_xir_array_get(&outer, 0, &f.local, &inner, &fault) == XR_XIR_VALUE_OK);
    CHECK(inner.payload != inner_payload && object_pointer(&inner)->domain == f.domains[0]);
    CHECK(xr_xir_array_get(&inner, 0, &f.local, &item, &fault) == XR_XIR_VALUE_OK);
    cross_extra_some(&item, &f); xr_xir_value_drop(&item);
    CHECK(xr_xir_cell_write(&f.cell, &inner, &f.local) == XR_XIR_VALUE_OK);
    xr_xir_value_drop(&inner); xr_xir_value_drop(&outer); xr_xir_value_drop(&some);
    printf("CROSS_DOMAIN_EXTRA mode=nested order=%s path_depth=2 published=Some(Function) inner_owner=A host_roots=0(next)\n", order ? "B-A" : "A-B");
    fflush(stdout);
    cross_extra_finish(&f, "nested", order);
}

static void cross_domain_set_case(unsigned order) {
    const XrXirCallableParameter fields[] = {{(XrXirType)257, 0}, {XR_XIR_STRING, 0}};
    const XrXirTypeNode nodes[] = {
        {.kind = XR_XIR_TYPE_CALLABLE, .result = XR_XIR_I64, .flags = XR_XIR_CALLABLE_ROOT_UNRESOLVED},
        {.kind = XR_XIR_TYPE_NULLABLE, .element = (XrXirType)256},
        {.kind = XR_XIR_TYPE_TUPLE, .parameters = fields, .parameter_count = 2},
        {.kind = XR_XIR_TYPE_ARRAY, .element = (XrXirType)258},
        {.kind = XR_XIR_TYPE_CELL, .element = (XrXirType)259}};
    const XrXirTypes types = {nodes, 5, NULL, NULL}; CrossExtra f;
    cross_extra_begin(&f, &types, (XrXirType)259, (XrXirType)260);
    XrXirValue none = {0}, old_text = {0}, old_tuple = {0}, array = {0};
    XrXirValue some = {0}, new_text = {0}, new_tuple = {0}, observed = {0}, field = {0};
    CHECK(xr_xir_nullable_new((XrXirType)257, NULL, &f.remote, &none) == XR_XIR_VALUE_OK);
    CHECK(xr_xir_string_new(f.domains[1], "old", 3, &old_text) == XR_XIR_VALUE_OK);
    XrXirValue old_fields[] = {none, old_text};
    CHECK(xr_xir_tuple_new((XrXirType)258, old_fields, 2, &f.remote, &old_tuple) == XR_XIR_VALUE_OK);
    CHECK(xr_xir_array_new((XrXirType)259, &old_tuple, 1, &f.remote, &array) == XR_XIR_VALUE_OK);
    xr_xir_value_drop(&old_tuple); xr_xir_value_drop(&none);
    CHECK(atomic_load(&object_pointer(&old_text)->references) == 2); /* host + old owned slot */
    CHECK(xr_xir_nullable_new((XrXirType)257, &f.function, &f.local, &some) == XR_XIR_VALUE_OK);
    CHECK(xr_xir_string_new(f.domains[0], "new", 3, &new_text) == XR_XIR_VALUE_OK);
    XrXirValue new_fields[] = {some, new_text};
    CHECK(xr_xir_tuple_new((XrXirType)258, new_fields, 2, &f.local, &new_tuple) == XR_XIR_VALUE_OK);
    int64_t payload = array.payload; XrXirValuePlace place = {(XrXirType)259, &array.payload};
    XrXirFaultDetail fault = {0};
    CHECK(xr_xir_array_set(&place, 0, &new_tuple, &f.local, &fault) == XR_XIR_VALUE_OK);
    CHECK(array.payload != payload && object_pointer(&array)->domain == f.domains[0]);
    CHECK(atomic_load(&object_pointer(&old_text)->references) == 1); /* old slot really released */
    cross_extra_text(&old_text, "old");
    CHECK(xr_xir_array_get(&array, 0, &f.local, &observed, &fault) == XR_XIR_VALUE_OK);
    CHECK(xr_xir_tuple_get(&observed, 0, &field) == XR_XIR_VALUE_OK);
    cross_extra_some(&field, &f); xr_xir_value_drop(&field);
    CHECK(xr_xir_tuple_get(&observed, 1, &field) == XR_XIR_VALUE_OK);
    cross_extra_text(&field, "new"); xr_xir_value_drop(&field); xr_xir_value_drop(&observed);
    CHECK(xr_xir_cell_write(&f.cell, &array, &f.local) == XR_XIR_VALUE_OK);
    xr_xir_value_drop(&old_text); xr_xir_value_drop(&new_text); xr_xir_value_drop(&new_tuple);
    xr_xir_value_drop(&some); xr_xir_value_drop(&array);
    printf("CROSS_DOMAIN_EXTRA mode=set order=%s published=Some(Function),new old_slot_released=1 array_owner=A host_roots=0(next)\n", order ? "B-A" : "A-B");
    fflush(stdout);
    cross_extra_finish(&f, "set", order);
}

static void cross_domain_extra_cases(void) {
    for (unsigned order = 0; order < 2; ++order) cross_domain_nested_case(order);
    for (unsigned order = 0; order < 2; ++order) cross_domain_set_case(order);
}
#endif // XIR_CROSS_DOMAIN_EXTRA_CASES_H
