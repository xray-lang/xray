/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xreflect_type.c - Type class methods
 */

#include "../../base/xchecks.h"
#include "../../base/xlog.h"
#include "../gc/xgc.h"
#include "xreflect_internal.h"
#include "../xisolate_api.h"
#include "xreflect_cache.h"
#include "../xvm_call.h"

// The per-class reflection cache is eagerly built by
// xr_class_builder_finalize, so the read path is just a field load.
// Classes created through paths that bypass the builder (or in early
// bootstrap where the isolate had no registry yet) may still have a
// NULL cache; every caller below keeps its pre-existing defensive
// branch to create wrapper objects on the fly in that case.

XrValue xr_type_getFields(XrayIsolate *isolate, XrValue *args, int nargs) {
    XR_DCHECK(isolate != NULL, "type_getFields: NULL isolate");
    if (nargs < 1) {
        xr_log_warning("reflect", "Type.getFields: requires this argument");
        return xr_null();
    }

    XrayIsolate *X = isolate;
    XrTypeMetadata *meta = xr_get_type_metadata(args[0]);
    if (!meta) return xr_null();

    XrArray *array = xr_array_new(xr_current_coro(X));

    // Traverse inheritance chain to get all fields
    XrClass *current_class = meta->klass;
    while (current_class != NULL) {
        for (int i = 0; i < current_class->own_field_count; i++) {
            XrFieldMetadata field_meta = { .owner = current_class, .field_index = i };
            XrValue field_obj = xr_create_field_object(X, &field_meta);
            xr_array_push(array, field_obj);
        }
        current_class = current_class->super;
    }

    return xr_value_from_array(array);
}

XrValue xr_type_getDeclaredFields(XrayIsolate *isolate, XrValue *args, int nargs) {
    XR_DCHECK(isolate != NULL, "type_getDeclaredFields: NULL isolate");
    if (nargs < 1) {
        xr_log_warning("reflect", "Type.getDeclaredFields: requires this argument");
        return xr_null();
    }

    XrayIsolate *X = isolate;
    XrTypeMetadata *meta = xr_get_type_metadata(args[0]);
    if (!meta || !meta->klass) return xr_null();

    // Use reflection cache for performance
    XrReflectCache *cache = meta->klass->reflect_cache;
    if (cache && cache->initialized) {
        XrArray *array = xr_array_new(xr_current_coro(X));
        // Only this class's declared fields (own_field_count)
        for (int i = 0; i < meta->klass->own_field_count && i < cache->field_count; i++) {
            xr_array_push(array, cache->field_wrappers[i]);
        }
        return xr_value_from_array(array);
    }

    // Fallback: create new objects (slower path)
    XrArray *array = xr_array_new(xr_current_coro(X));
    for (int i = 0; i < meta->klass->own_field_count; i++) {
        XrFieldMetadata field_meta = { .owner = meta->klass, .field_index = i };
        XrValue field_obj = xr_create_field_object(X, &field_meta);
        xr_array_push(array, field_obj);
    }

    return xr_value_from_array(array);
}

XrValue xr_type_getField(XrayIsolate *isolate, XrValue *args, int nargs) {
    XR_DCHECK(isolate != NULL, "type_getField: NULL isolate");
    if (nargs < 2) {
        xr_log_warning("reflect", "Type.getField: requires 2 arguments");
        return xr_null();
    }

    if (!XR_IS_STRING(args[1])) {
        xr_log_warning("reflect", "Type.getField: argument must be string");
        return xr_null();
    }

    XrayIsolate *X = isolate;
    XrTypeMetadata *meta = xr_get_type_metadata(args[0]);
    XrString *name_str = XR_TO_STRING(args[1]);

    if (!meta || !meta->klass) return xr_null();

    // Search inheritance chain
    XrClass *current_class = meta->klass;
    while (current_class != NULL) {
        // Try to use cache for this class (may be NULL for classes
        // that finalised before the isolate's registry was ready).
        XrReflectCache *cache = current_class->reflect_cache;

        for (int i = 0; i < current_class->own_field_count; i++) {
            if (current_class->fields && current_class->fields[i].name &&
                strcmp(current_class->fields[i].name, name_str->data) == 0) {
                // Return cached wrapper if available
                if (cache && cache->initialized && i < cache->field_count) {
                    return cache->field_wrappers[i];
                }
                // Fallback: create new object
                XrFieldMetadata field_meta = { .owner = current_class, .field_index = i };
                return xr_create_field_object(X, &field_meta);
            }
        }
        current_class = current_class->super;
    }

    return xr_null();
}

/**
 * Type.getMethods(): Array<Method>
 */

XrValue xr_type_getMethods(XrayIsolate *isolate, XrValue *args, int nargs) {
    XR_DCHECK(isolate != NULL, "type_getMethods: NULL isolate");
    if (nargs < 1) {
        xr_log_warning("reflect", "Type.getMethods: requires this argument");
        return xr_null();
    }

    XrayIsolate *X = isolate;
    XrTypeMetadata *meta = xr_get_type_metadata(args[0]);
    if (!meta || !meta->klass) return xr_null();

    // Use reflection cache for performance (50-100x speedup)
    XrReflectCache *cache = meta->klass->reflect_cache;
    if (cache && cache->initialized) {
        XrArray *array = xr_array_new(xr_current_coro(X));
        for (int i = 0; i < cache->method_count; i++) {
            xr_array_push(array, cache->method_wrappers[i]);
        }
        return xr_value_from_array(array);
    }

    // Fallback: create new objects (slower path)
    XrArray *array = xr_array_new(xr_current_coro(X));
    for (int i = 0; i < meta->klass->method_count; i++) {
        XrMethodMetadata method_meta = { .owner = meta->klass, .method_index = i };
        XrValue method_obj = xr_create_method_object(X, &method_meta);
        xr_array_push(array, method_obj);
    }

    return xr_value_from_array(array);
}

/**
 * Type.getDeclaredMethods(): Array<Method>
 * Returns only methods declared in this class (not inherited)
 */

XrValue xr_type_getDeclaredMethods(XrayIsolate *isolate, XrValue *args, int nargs) {
    XR_DCHECK(isolate != NULL, "type_getDeclaredMethods: NULL isolate");
    if (nargs < 1) {
        xr_log_warning("reflect", "Type.getDeclaredMethods: requires this argument");
        return xr_null();
    }

    XrayIsolate *X = isolate;
    XrTypeMetadata *meta = xr_get_type_metadata(args[0]);
    if (!meta || !meta->klass) return xr_null();

    XrArray *array = xr_array_new(xr_current_coro(X));
    XrClass *klass = meta->klass;

    // Use reflection cache for performance
    XrReflectCache *cache = klass->reflect_cache;

    // Filter to only declared methods (starting from own_method_start in vtable)
    // For methods array, we check if the method's declaring class is this class
    for (int i = 0; i < klass->method_count; i++) {
        XrMethod *m = &klass->methods[i];
        // Skip if method is inherited (vtable_index < own_method_start indicates inherited)
        // For simplicity, we include all methods in this class's method array
        // The method array already contains only this class's methods by design

        if (cache && cache->initialized && i < cache->method_count) {
            xr_array_push(array, cache->method_wrappers[i]);
        } else {
            XrMethodMetadata method_meta = { .owner = klass, .method_index = i };
            XrValue method_obj = xr_create_method_object(X, &method_meta);
            xr_array_push(array, method_obj);
        }
        (void)m;
    }

    return xr_value_from_array(array);
}

/**
 * Type.getMethod(name: string): Method?
 */

XrValue xr_type_getMethod(XrayIsolate *isolate, XrValue *args, int nargs) {
    XR_DCHECK(isolate != NULL, "type_getMethod: NULL isolate");
    if (nargs < 2) {
        xr_log_warning("reflect", "Type.getMethod: requires 2 arguments");
        return xr_null();
    }

    if (!XR_IS_STRING(args[1])) {
        xr_log_warning("reflect", "Type.getMethod: argument must be string");
        return xr_null();
    }

    XrayIsolate *X = isolate;
    XrTypeMetadata *meta = xr_get_type_metadata(args[0]);
    XrString *name_str = XR_TO_STRING(args[1]);

    if (!meta || !meta->klass) return xr_null();

    // Use reflection cache for performance
    XrReflectCache *cache = meta->klass->reflect_cache;

    XrSymbolTable *sym_table = (XrSymbolTable*)xr_isolate_get_symbol_table(X);
    for (int i = 0; i < meta->klass->method_count; i++) {
        const char *method_name = xr_symbol_get_name_in_table(sym_table, meta->klass->methods[i].symbol);
        if (method_name && strcmp(method_name, name_str->data) == 0) {
            // Return cached wrapper if available
            if (cache && cache->initialized && i < cache->method_count) {
                return cache->method_wrappers[i];
            }
            // Fallback: create new object
            XrMethodMetadata method_meta = { .owner = meta->klass, .method_index = i };
            return xr_create_method_object(X, &method_meta);
        }
    }

    return xr_null();
}

/**
 * Type.getConstructor(): Constructor?
 */

XrValue xr_type_getConstructor(XrayIsolate *isolate, XrValue *args, int nargs) {
    XR_DCHECK(isolate != NULL, "type_getConstructor: NULL isolate");
    if (nargs < 1) {
        xr_log_warning("reflect", "Type.getConstructor: requires this argument");
        return xr_null();
    }

    XrayIsolate *X = isolate;
    XrTypeMetadata *meta = xr_get_type_metadata(args[0]);
    if (!meta) return xr_null();

    XrClass *klass = meta->klass;
    if (!klass) return xr_null();

    // Search for constructor method in class
    for (int i = 0; i < klass->method_count; i++) {
        if (klass->methods[i].flags & XMETHOD_FLAG_CONSTRUCTOR) {
            XrMethodMetadata ctor_meta = { .owner = klass, .method_index = i };
            return xr_create_constructor_object(X, &ctor_meta);
        }
    }
    return xr_null();
}

/**
 * Type.newInstance(): any
 */

XrValue xr_type_newInstance(XrayIsolate *isolate, XrValue *args, int nargs) {
    XR_DCHECK(isolate != NULL, "type_newInstance: NULL isolate");
    if (nargs < 1) {
        xr_log_warning("reflect", "Type.newInstance: requires this argument");
        return xr_null();
    }

    XrayIsolate *X = isolate;
    XrTypeMetadata *meta = xr_get_type_metadata(args[0]);
    if (!meta || !meta->klass) return xr_null();

    XrInstance *instance = xr_instance_new(X, meta->klass);
    return xr_value_from_instance(instance);
}

XrValue xr_type_newInstanceWith(XrayIsolate *isolate, XrValue *args, int nargs) {
    XR_DCHECK(isolate != NULL, "type_newInstanceWith: NULL isolate");
    if (nargs < 2) {
        xr_log_warning("reflect", "Type.newInstanceWith: requires 2 arguments");
        return xr_null();
    }

    XrayIsolate *X = isolate;
    XrTypeMetadata *meta = xr_get_type_metadata(args[0]);
    if (!meta || !meta->klass) return xr_null();

    XrInstance *instance = xr_instance_new(X, meta->klass);
    XrValue instance_val = xr_value_from_instance(instance);

    XrSymbolTable *sym_table = (XrSymbolTable*)xr_isolate_get_symbol_table(X);
    int constructor_symbol = xr_symbol_lookup_in_table(sym_table, XR_KEYWORD_CONSTRUCTOR);

    if (constructor_symbol > 0) {
        for (int i = 0; i < meta->klass->method_count; i++) {
            XrMethod *method = &meta->klass->methods[i];
            if (method->symbol == constructor_symbol) {
                XrArray *arg_array = NULL;
                int arg_count = 0;

                if (XR_IS_ARRAY(args[1])) {
                    arg_array = XR_TO_ARRAY(args[1]);
                    arg_count = arg_array->length;
                }

                XrValue call_args[256];
                int total_args = 0;

                call_args[total_args++] = instance_val;

                if (arg_array && arg_count > 0) {
                    XrValue *adata = (XrValue*)arg_array->data;
                    for (int j = 0; j < arg_count && total_args < 256; j++) {
                        call_args[total_args++] = adata[j];
                    }
                }

                if (method->type == XMETHOD_PRIMITIVE) {
                    XrCFunctionPtr func = method->as.primitive;
                    func(X, call_args, total_args);
                } else if (method->type == XMETHOD_CLOSURE && method->as.closure) {
                    xr_vm_call_closure(X, method->as.closure, call_args, total_args);
                }

                break;
            }
        }
    }

    return instance_val;
}

XrValue xr_type_isSubtypeOf(XrayIsolate *isolate, XrValue *args, int nargs) {
    (void)isolate;
    if (nargs < 2) {
        xr_log_warning("reflect", "Type.isSubtypeOf: requires 2 arguments");
        return xr_bool(false);
    }

    XrTypeMetadata *this_meta = xr_get_type_metadata(args[0]);
    XrTypeMetadata *other_meta = xr_get_type_metadata(args[1]);

    if (!this_meta || !other_meta) return xr_bool(false);
    if (!this_meta->klass || !other_meta->klass) return xr_bool(false);

    bool result = xr_class_instanceof(this_meta->klass, other_meta->klass);
    return xr_bool(result);
}

XrValue xr_type_isAssignableFrom(XrayIsolate *isolate, XrValue *args, int nargs) {
    (void)isolate;
    if (nargs < 2) {
        xr_log_warning("reflect", "Type.isAssignableFrom: requires 2 arguments");
        return xr_bool(false);
    }

    XrTypeMetadata *this_meta = xr_get_type_metadata(args[0]);
    XrTypeMetadata *other_meta = xr_get_type_metadata(args[1]);

    if (!this_meta || !other_meta) return xr_bool(false);
    if (!this_meta->klass || !other_meta->klass) return xr_bool(false);

    bool result = xr_class_instanceof(other_meta->klass, this_meta->klass);
    return xr_bool(result);
}

XrValue xr_type_implements(XrayIsolate *isolate, XrValue *args, int nargs) {
    (void)isolate;
    if (nargs < 2) {
        xr_log_warning("reflect", "Type.implements: requires 2 arguments");
        return xr_bool(false);
    }

    // Get the type being checked
    XrTypeMetadata *meta = xr_get_type_metadata(args[0]);
    if (!meta || !meta->klass) return xr_bool(false);

    // Get the interface to check against
    XrTypeMetadata *iface_meta = xr_get_type_metadata(args[1]);
    if (!iface_meta || !iface_meta->klass) return xr_bool(false);

    // Check if the interface flag is set on the target
    if (!(iface_meta->klass->flags & XR_CLASS_INTERFACE)) {
        return xr_bool(false);  // Not an interface
    }

    // Use xr_class_implements_interface_fast for O(n) check
    return xr_bool(xr_class_implements_interface_fast(meta->klass, iface_meta->klass));
}

/* ========== Type Property Getters ========== */

XrValue xr_type_getName(XrayIsolate *isolate, XrValue *args, int nargs) {
    XR_DCHECK(isolate != NULL, "type_getName: NULL isolate");
    if (nargs < 1) return xr_null();

    XrTypeMetadata *meta = xr_get_type_metadata(args[0]);
    if (!meta) return xr_null();

    const char *type_name = meta->klass ? meta->klass->name : meta->name;
    if (!type_name) return xr_null();

    XrString *name_str = xr_string_intern(isolate, type_name, strlen(type_name), 0);
    return xr_string_value(name_str);
}

XrValue xr_type_getKind(XrayIsolate *isolate, XrValue *args, int nargs) {
    XR_DCHECK(isolate != NULL, "type_getKind: NULL isolate");
    if (nargs < 1) return xr_null();

    XrTypeMetadata *meta = xr_get_type_metadata(args[0]);
    if (!meta) return xr_null();

    const char *kind_str = "class";
    if (meta->klass) {
        if (meta->klass->flags & XR_CLASS_INTERFACE) {
            kind_str = "interface";
        } else if (meta->klass->flags & XR_CLASS_ABSTRACT) {
            kind_str = "abstract";
        }
    }

    XrString *kind = xr_string_intern(isolate, kind_str, strlen(kind_str), 0);
    return xr_string_value(kind);
}

XrValue xr_type_getIsAbstract(XrayIsolate *isolate, XrValue *args, int nargs) {
    (void)isolate;
    if (nargs < 1) return xr_bool(false);

    XrTypeMetadata *meta = xr_get_type_metadata(args[0]);
    if (!meta || !meta->klass) return xr_bool(false);

    return xr_bool((meta->klass->flags & XR_CLASS_ABSTRACT) != 0);
}

XrValue xr_type_getIsFinal(XrayIsolate *isolate, XrValue *args, int nargs) {
    (void)isolate;
    if (nargs < 1) return xr_bool(false);

    XrTypeMetadata *meta = xr_get_type_metadata(args[0]);
    if (!meta || !meta->klass) return xr_bool(false);

    return xr_bool((meta->klass->flags & XR_CLASS_FINAL) != 0);
}

XrValue xr_type_getSuperType(XrayIsolate *isolate, XrValue *args, int nargs) {
    XR_DCHECK(isolate != NULL, "type_getSuperType: NULL isolate");
    if (nargs < 1) return xr_null();

    if (!XR_IS_INSTANCE(args[0])) return xr_null();
    TypeWrapper *wrapper = (TypeWrapper*)XR_TO_INSTANCE(args[0]);
    XrTypeMetadata *meta = &wrapper->metadata;

    if (!meta->klass) return xr_null();

    XrClass *super = meta->klass->super;
    if (!super) return xr_null();

    XrTypeMetadata super_meta = { .klass = super, .name = NULL };
    return xr_create_type_object(isolate, &super_meta);
}
