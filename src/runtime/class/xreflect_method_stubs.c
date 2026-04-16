/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xreflect_method_stubs.c - Method/Field helper functions (zero-copy)
 */

#include "xreflect_internal.h"
#include "../../base/xchecks.h"
#include "xmethod.h"
#include "xclass.h"
#include "../object/xstring.h"

static inline XrMethodMetadata* get_method_meta(XrValue val) {
    if (!XR_IS_INSTANCE(val)) return NULL;
    MethodWrapper *wrapper = (MethodWrapper*)XR_TO_INSTANCE(val);
    return &wrapper->metadata;
}

static inline XrFieldMetadata* get_field_meta(XrValue val) {
    if (!XR_IS_INSTANCE(val)) return NULL;
    FieldWrapper *wrapper = (FieldWrapper*)XR_TO_INSTANCE(val);
    return &wrapper->metadata;
}

/* ========== Method Property Getters ========== */

XrValue xr_method_getIsStatic(XrayIsolate *X, XrValue *args, int nargs) {
    (void)X;
    if (nargs < 1) return xr_bool(false);
    
    XrMethodMetadata *meta = get_method_meta(args[0]);
    if (!meta || !meta->owner) return xr_bool(false);
    
    XrMethod *method = &meta->owner->methods[meta->method_index];
    return xr_bool((method->flags & XMETHOD_FLAG_STATIC) != 0);
}

XrValue xr_method_getIsPrivate(XrayIsolate *X, XrValue *args, int nargs) {
    (void)X;
    if (nargs < 1) return xr_bool(false);
    
    XrMethodMetadata *meta = get_method_meta(args[0]);
    if (!meta || !meta->owner) return xr_bool(false);
    
    XrMethod *method = &meta->owner->methods[meta->method_index];
    return xr_bool((method->flags & XMETHOD_FLAG_PRIVATE) != 0);
}

XrValue xr_method_getIsAbstract(XrayIsolate *X, XrValue *args, int nargs) {
    (void)X;
    if (nargs < 1) return xr_bool(false);
    
    XrMethodMetadata *meta = get_method_meta(args[0]);
    if (!meta || !meta->owner) return xr_bool(false);
    
    XrMethod *method = &meta->owner->methods[meta->method_index];
    return xr_bool((method->flags & XMETHOD_FLAG_ABSTRACT) != 0);
}

/* ========== Field Property Getters ========== */

XrValue xr_field_getDeclaringType(XrayIsolate *X, XrValue *args, int nargs) {
    if (nargs < 1) return xr_null();
    
    XrFieldMetadata *meta = get_field_meta(args[0]);
    if (!meta || !meta->owner) return xr_null();
    
    XrTypeMetadata type_meta = { .klass = meta->owner };
    return xr_create_type_object(X, &type_meta);
}

XrValue xr_field_getIsPrivate(XrayIsolate *X, XrValue *args, int nargs) {
    (void)X;
    if (nargs < 1) return xr_bool(false);
    
    XrFieldMetadata *meta = get_field_meta(args[0]);
    if (!meta || !meta->owner) return xr_bool(false);
    
    return xr_bool(xr_class_is_field_private(meta->owner, meta->field_index));
}
