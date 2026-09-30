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
typedef struct XirClassObject { XirObject object; } XirClassObject;
_Static_assert(_Alignof(XirClassObject) >= _Alignof(int64_t), "class body alignment");
_Static_assert((uint32_t)XR_XIR_TYPE_NOMINAL < (uint32_t)XIR_OBJECT_CLASS, "class object tag is not a type-node kind");
static const XrXirStorageLayout *class_body_layout(const XirObject *object) {
    if (!object || object->kind != XIR_OBJECT_CLASS ||
        !xr_xir_type_is_class(xr_xir_type_arena_types(object->arena),object->type)) return NULL;
    const XrXirStorageLayout *layout=xr_xir_type_arena_storage(object->arena,object->type);
    return layout && layout->value.size == sizeof(uint64_t) &&
        layout->body.alignment && layout->body.alignment <= _Alignof(XirClassObject) ? layout : NULL;
}
static XrXirValue class_field_value(const XirObject *object, uint32_t field) {
    const XrXirTypeNode *node=xr_xir_type_node(xr_xir_type_arena_types(object->arena),object->type);
    const XrXirStorageLayout *layout=class_body_layout(object);
    XrXirLayout physical={0};
    XR_CHECK(node && layout && field < node->nominal.field_count &&
        xr_xir_type_arena_layout(object->arena,node->nominal.fields[field],&physical),
        "class field requires sealed body metadata");
    const unsigned char *body=(const unsigned char *)((const XirClassObject *)object+1);
    return storage_leaf_value(node->nominal.fields[field],body+layout->field_offsets[field],physical.size);
}
static void class_field_publish(XirObject *object, uint32_t field, const XrXirValue *value) {
    const XrXirStorageLayout *layout=class_body_layout(object);
    XrXirLayout physical={0};
    XR_CHECK(layout && field < layout->field_count &&
        xr_xir_type_arena_layout(object->arena,(XrXirType)value->type,&physical),
        "class field publication requires sealed leaf metadata");
    unsigned char *body=(unsigned char *)((XirClassObject *)object+1);
    memcpy(body+layout->field_offsets[field],&value->payload,physical.size);
}
static bool inline_nominal_type(const XrXirTypes *types, XrXirType type) {
    return xr_xir_type_is_nominal(types,type) && !xr_xir_type_is_class(types,type);
}
