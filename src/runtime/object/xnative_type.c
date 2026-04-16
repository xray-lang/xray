/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xnative_type.c - Native type registration system implementation
 *
 * KEY CONCEPT:
 *   Registers C native types as XrClass for method dispatch.
 */

#include "xnative_type.h"
#include "../class/xclass_builder.h"
#include "../xisolate_api.h"
#include "../../base/xmalloc.h"
#include "../../base/xchecks.h"
#include <string.h>

/* ========== Registration API Implementation ========== */

// Register native type, generate corresponding XrClass
XrClass* xr_register_native_type(XrayIsolate *isolate, const XrNativeTypeInfo *info) {
    XR_DCHECK(isolate != NULL, "Isolate must not be NULL");
    XR_DCHECK(info != NULL, "NativeTypeInfo must not be NULL");
    XR_DCHECK(info->name != NULL, "Type name must not be NULL");
    XR_DCHECK(info->gc_type < XR_NATIVE_TYPE_MAX, "GC type out of range");
    
    // Check if already registered
    XrClass *existing = xr_isolate_get_native_type_class(isolate, info->gc_type);
    if (existing != NULL) {
        // Already registered, return existing class
        return existing;
    }
    
    // Use ClassBuilder to create class
    XrClassBuilder *builder = xr_class_builder_new(isolate, info->name, NULL);
    if (!builder) {
        return NULL;
    }
    
    // Add instance methods
    if (info->methods) {
        for (int i = 0; info->methods[i].name != NULL; i++) {
            const XrNativeMethod *m = &info->methods[i];
            xr_class_builder_add_method(
                builder,
                m->name,
                m->func,
                m->arity,
                0  // flags: instance method
            );
        }
    }
    
    // Add property getters (as parameterless methods)
    if (info->getters) {
        for (int i = 0; info->getters[i].name != NULL; i++) {
            const XrNativeMethod *g = &info->getters[i];
            xr_class_builder_add_method(
                builder,
                g->name,
                g->func,
                1,  // arity: only this
                0
            );
        }
    }
    
    // Add static methods
    if (info->static_methods) {
        for (int i = 0; info->static_methods[i].name != NULL; i++) {
            const XrNativeMethod *s = &info->static_methods[i];
            xr_class_builder_add_method(
                builder,
                s->name,
                s->func,
                s->arity,
                XMETHOD_FLAG_STATIC
            );
        }
    }
    
    // Finalize build
    XrClass *klass = xr_class_builder_finalize(builder);
    if (!klass) {
        return NULL;
    }
    
    // Mark as built-in class
    klass->flags |= XR_CLASS_BUILTIN;
    
    // Register to mapping table
    xr_isolate_set_native_type_class(isolate, info->gc_type, klass);
    
    return klass;
}

// Get XrClass for native type
XrClass* xr_get_native_type_class(XrayIsolate *isolate, XrObjType gc_type) {
    if (!isolate || gc_type >= XR_NATIVE_TYPE_MAX) {
        return NULL;
    }
    return xr_isolate_get_native_type_class(isolate, gc_type);
}

// Check if type is registered
bool xr_is_native_type_registered(XrayIsolate *isolate, XrObjType gc_type) {
    return xr_get_native_type_class(isolate, gc_type) != NULL;
}

