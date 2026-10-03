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
enum { XIR_OBJECT_CLASS = 0x100u };
typedef struct XirClassObject {
    XirObject object;
    size_t allocation_bytes, release_offset;
    uint32_t release_capacity;
} XirClassObject;
_Static_assert(_Alignof(XirClassObject) >= _Alignof(int64_t), "class body alignment");
_Static_assert((uint32_t)XR_XIR_TYPE_NOMINAL < (uint32_t)XIR_OBJECT_CLASS, "class object tag is not a type-node kind");
static const XrXirStorageLayout *class_body_layout(const XirObject *object) {
    if (!object || object->kind != XIR_OBJECT_CLASS ||
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
