/*
 * xray - Lightweight typed scripting with native concurrency
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_array_set_callback_case.h - Published same-domain SET at release callback
 */
#ifndef XIR_ARRAY_SET_CALLBACK_CASE_H
#define XIR_ARRAY_SET_CALLBACK_CASE_H
typedef struct ArraySetObserver {
    ForeignArray *fixture;
    int64_t backing;
    size_t calls;
    bool mutating;
} ArraySetObserver;

static void array_set_observe_release(void *owner) {
    ArraySetObserver *observer = owner; ForeignArray *f = observer->fixture;
    CHECK(observer->mutating && !observer->calls && f->array.payload == observer->backing);
    CHECK(object_pointer(&f->array)->domain == f->domains[0]);
    ++observer->calls;
    XrXirValue tuple = {0}, field = {0}; XrXirFaultDetail fault = {0};
    int64_t length = -1;
    CHECK(xr_xir_array_len(&f->array, &f->local, &length) == XR_XIR_VALUE_OK && length == 1);
    CHECK(xr_xir_array_get(&f->array, 0, &f->local, &tuple, &fault) == XR_XIR_VALUE_OK && !fault.code);
    CHECK(tuple.payload == f->replacement.payload);
    CHECK(xr_xir_tuple_get(&tuple, 0, &field) == XR_XIR_VALUE_OK);
    bool some = false; const XrXirValue *payload = NULL;
    CHECK(xr_xir_nullable_view(&field, &some, &payload) && some && payload && payload->payload == f->function.payload);
    xr_xir_value_drop(&field);
    CHECK(xr_xir_tuple_get(&tuple, 1, &field) == XR_XIR_VALUE_OK);
    cross_extra_text(&field, "new");
    xr_xir_value_drop(&field); xr_xir_value_drop(&tuple);
}

static XrXirValueStatus array_set_observer_admit(void *context, const XrXirFunctionBinding *binding,
    XrXirType type, uint64_t *work) {
    ArraySetObserver *observer = context;
    if (binding->owner == &observer->fixture->releases)
        return capture_admit(&observer->fixture->releases, binding, type, work);
    if (!*work) return XR_XIR_VALUE_LIMIT;
    --*work;
    return type == (XrXirType)256 && binding->owner == observer && binding->release == array_set_observe_release &&
        binding->entry == 0 && !binding->capture_count ? XR_XIR_VALUE_OK : XR_XIR_VALUE_BAD_ARGUMENT;
}

static void array_set_callback_case(void) {
    ForeignArray f; foreign_array_prepare(&f);
    xr_xir_value_drop(&f.array); /* Replace the initial B fixture with a legal A graph. */
    CHECK(atomic_load(&object_pointer(&f.old_text)->references) == 1);
    ArraySetObserver observer = {.fixture = &f};
    f.local.function = array_set_observer_admit; f.local.context = &observer;
    XrXirValue old_function = {0}, old_some = {0}, old_tuple = {0};
    XrXirFunctionBinding binding = {&observer, array_set_observe_release, 0, NULL, 0};
    CHECK(xr_xir_function_new(f.domains[0], f.arena, (XrXirType)256, &binding, &f.local, &old_function) == XR_XIR_VALUE_OK);
    CHECK(xr_xir_nullable_new((XrXirType)257, &old_function, &f.local, &old_some) == XR_XIR_VALUE_OK);
    XrXirValue fields[] = {old_some, f.old_text};
    CHECK(xr_xir_tuple_new((XrXirType)258, fields, 2, &f.local, &old_tuple) == XR_XIR_VALUE_OK);
    CHECK(xr_xir_array_new((XrXirType)259, &old_tuple, 1, &f.local, &f.array) == XR_XIR_VALUE_OK);
    xr_xir_value_drop(&old_tuple); xr_xir_value_drop(&old_some); xr_xir_value_drop(&old_function);
    CHECK(!observer.calls && atomic_load(&object_pointer(&f.array)->references) == 1);
    CHECK(atomic_load(&object_pointer(&f.old_text)->references) == 2);
    observer.backing = f.array.payload; observer.mutating = true;
    XrXirValuePlace place = {(XrXirType)259, &f.array.payload}; XrXirFaultDetail fault = {0};
    CHECK(xr_xir_array_set(&place, 0, &f.replacement, &f.local, &fault) == XR_XIR_VALUE_OK && !fault.code);
    observer.mutating = false;
    CHECK(observer.calls == 1 && !f.releases && f.array.payload == observer.backing);
    CHECK(atomic_load(&object_pointer(&f.old_text)->references) == 1);
    foreign_array_observe(&f, false, true);
    size_t cleanup; closed_no_alloc_begin(&cleanup);
    xr_xir_compile_type_arena_drop(f.arena);
    xr_xir_value_drop(&f.array); xr_xir_value_drop(&f.replacement); xr_xir_value_drop(&f.some);
    xr_xir_value_drop(&f.function); xr_xir_value_drop(&f.old_text); xr_xir_value_drop(&f.new_text);
    xr_xir_domain_close(f.domains[0]); xr_xir_domain_close(f.domains[1]);
    xr_xir_domain_drop(f.domains[0]); xr_xir_domain_drop(f.domains[1]);
    CHECK(observer.calls == 1 && f.releases == 1 && !live && !value_compile_live && !value_compile_bytes);
    closed_no_alloc_end(cleanup);
    printf("ARRAY_SET_CALLBACK same_domain=1 same_backing=1 callback=1 observed_new=1 physical=0/0 cleanup=0\n");
}
#endif // XIR_ARRAY_SET_CALLBACK_CASE_H
