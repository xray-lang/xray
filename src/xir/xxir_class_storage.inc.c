/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_class_storage.inc.c - Physical class bodies behind identity handles
 *
 * KEY CONCEPT:
 *   Identity handles pin their field storage, type arena and physical accounting domain.
 */
typedef struct XirClassObject {
    XirObject object;
    size_t allocation_bytes, release_offset;
    uint32_t release_capacity;
    _Atomic(uint32_t) publication_state;
    XrXirCellLoan *borrow_top;
    uint64_t observation_serial;
    uint32_t observation_state, observation_declaration;
} XirClassObject;
_Static_assert(offsetof(XirClassObject, publication_state) ==
    offsetof(XirClassObject, release_capacity) + sizeof(uint32_t), "Class origin uses the existing padding");
_Static_assert(offsetof(XirClassObject, borrow_top) ==
    offsetof(XirClassObject, publication_state) + sizeof(uint32_t), "Class origin preserves the loan offset");
_Static_assert(_Alignof(XirClassObject) >= _Alignof(int64_t), "class body alignment");
_Static_assert((uint32_t)XR_XIR_TYPE_NOMINAL < (uint32_t)XIR_OBJECT_CLASS, "class object tag is not a type-node kind");
static const XrXirStorageLayout *class_body_layout(const XirObject *object) {
    if (!object || object->kind != XIR_OBJECT_CLASS || !xr_xir_domain_current(object->domain) ||
        !xr_xir_type_is_class(xr_xir_compile_type_arena_types(object->arena),object->type)) return NULL;
    const XrXirStorageLayout *layout=xr_xir_compile_type_arena_storage(object->arena,object->type);
    return layout && layout->value.size == sizeof(uint64_t) &&
        layout->body.alignment && layout->body.alignment <= _Alignof(XirClassObject) ? layout : NULL;
}
static bool inline_nominal_type(const XrXirTypes *types, XrXirType type) {
    return xr_xir_type_is_nullable(types,type) ||
        (xr_xir_type_is_nominal(types,type) && !xr_xir_type_is_class(types,type));
}

static bool class_allocation_valid(const XirObject *object);
static uint32_t class_origin_state(const XirClassObject *object) {
    return atomic_load_explicit(&object->publication_state, memory_order_acquire);
}
/* A single reservation belongs to either a prepared publication or loan. */
static bool class_origin_reserve(XirClassObject *object, bool loan) {
    uint32_t state = class_origin_state(object);
    if ((state & ~(uint32_t)XIR_CLASS_ORIGIN_VALID) ||
        (state & XIR_CLASS_ORIGIN_PREPARED) ||
        (!loan && (state & XIR_CLASS_ORIGIN_BORROWED))) return false;
    return atomic_compare_exchange_strong_explicit(&object->publication_state,
        &state, state | XIR_CLASS_ORIGIN_PREPARED, memory_order_acq_rel, memory_order_acquire);
}
static void class_origin_cancel(XirClassObject *object) {
    uint32_t state = atomic_fetch_and_explicit(&object->publication_state,
        ~(uint32_t)XIR_CLASS_ORIGIN_PREPARED, memory_order_release);
    XR_CHECK(state & XIR_CLASS_ORIGIN_PREPARED, "only the actual prepared Class owner cancels its reservation");
}
