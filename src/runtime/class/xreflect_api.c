/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xreflect_api.c - Reflection API implementation
 */

#include "../../base/xchecks.h"
#include "../../base/xlog.h"
#include "../gc/xgc.h"
#include "xreflect_internal.h"
#include "xreflect_registry.h"
#include "../xisolate_api.h"
#include "xinstance.h"
#include "../object/xjson.h"
#include "../object/xarray.h"
#include "../object/xmap.h"
#include "../object/xset.h"
#include "../../coro/xchannel.h"
#include "../value/xtype_names.h"
#include "../value/xvalue.h"
#include "../value/xstruct_layout.h"

XrValue xr_create_type_object(XrayIsolate *X, XrTypeMetadata *meta) {
    XR_DCHECK(X != NULL, "create_type_object: NULL isolate");
    if (!meta)
        return xr_null();

    TypeWrapper *wrapper =
        (TypeWrapper *) xr_gc_alloc(xr_isolate_get_gc(X), sizeof(TypeWrapper), XR_TINSTANCE);
    if (!wrapper)
        return xr_null();

    XrClass *typeClass =
        xr_isolate_get_core_classes(X) ? xr_isolate_get_core_classes(X)->typeClass : NULL;
    (void) typeClass;
    xr_gc_header_init_type(&wrapper->gc, XR_TINSTANCE);
    wrapper->metadata = *meta;

    return wrapper_to_value(wrapper);
}

XrValue xr_create_field_object(XrayIsolate *X, XrFieldMetadata *field) {
    XR_DCHECK(X != NULL, "create_field_object: NULL isolate");
    if (!field)
        return xr_null();

    FieldWrapper *wrapper =
        (FieldWrapper *) xr_gc_alloc(xr_isolate_get_gc(X), sizeof(FieldWrapper), XR_TINSTANCE);
    if (!wrapper)
        return xr_null();

    XrClass *fieldClass =
        xr_isolate_get_core_classes(X) ? xr_isolate_get_core_classes(X)->fieldClass : NULL;
    (void) fieldClass;
    xr_gc_header_init_type(&wrapper->gc, XR_TINSTANCE);
    wrapper->metadata = *field;

    return wrapper_to_value(wrapper);
}

XrValue xr_create_method_object(XrayIsolate *X, XrMethodMetadata *method) {
    XR_DCHECK(X != NULL, "create_method_object: NULL isolate");
    if (!method)
        return xr_null();

    MethodWrapper *wrapper =
        (MethodWrapper *) xr_gc_alloc(xr_isolate_get_gc(X), sizeof(MethodWrapper), XR_TINSTANCE);
    if (!wrapper)
        return xr_null();

    XrClass *methodClass =
        xr_isolate_get_core_classes(X) ? xr_isolate_get_core_classes(X)->methodClass : NULL;
    (void) methodClass;
    xr_gc_header_init_type(&wrapper->gc, XR_TINSTANCE);
    wrapper->metadata = *method;

    return wrapper_to_value(wrapper);
}

XrValue xr_create_constructor_object(XrayIsolate *X, XrMethodMetadata *ctor) {
    return xr_create_method_object(X, ctor);
}

XrValue xr_create_parameter_object(XrayIsolate *X, XrParameterMetadata *param) {
    XR_DCHECK(X != NULL, "create_parameter_object: NULL isolate");
    if (!param)
        return xr_null();

    ParameterWrapper *wrapper = (ParameterWrapper *) xr_gc_alloc(
        xr_isolate_get_gc(X), sizeof(ParameterWrapper), XR_TINSTANCE);
    if (!wrapper)
        return xr_null();

    XrClass *paramClass =
        xr_isolate_get_core_classes(X) ? xr_isolate_get_core_classes(X)->parameterClass : NULL;
    (void) paramClass;
    xr_gc_header_init_type(&wrapper->gc, XR_TINSTANCE);
    wrapper->metadata = *param;

    return wrapper_to_value(wrapper);
}

/* ========== Metadata Extraction ========== */

XrTypeMetadata *xr_get_type_metadata(XrValue type_obj) {
    if (!XR_IS_INSTANCE(type_obj))
        return NULL;

    TypeWrapper *wrapper = (TypeWrapper *) XR_TO_INSTANCE(type_obj);
    return &wrapper->metadata;
}

XrFieldMetadata *xr_get_field_metadata(XrValue field_obj) {
    if (!XR_IS_INSTANCE(field_obj))
        return NULL;
    FieldWrapper *wrapper = (FieldWrapper *) XR_TO_INSTANCE(field_obj);
    return &wrapper->metadata;
}

XrMethodMetadata *xr_get_method_metadata(XrValue method_obj) {
    if (!XR_IS_INSTANCE(method_obj))
        return NULL;
    MethodWrapper *wrapper = (MethodWrapper *) XR_TO_INSTANCE(method_obj);
    return &wrapper->metadata;
}

XrParameterMetadata *xr_get_parameter_metadata(XrValue param_obj) {
    if (!XR_IS_INSTANCE(param_obj))
        return NULL;
    ParameterWrapper *wrapper = (ParameterWrapper *) XR_TO_INSTANCE(param_obj);
    return &wrapper->metadata;
}

/* ========== Reflect Class Methods ========== */

XrValue xr_reflect_getType(XrayIsolate *isolate, XrValue self, XrValue *args, int nargs) {
    (void) self;
    XR_DCHECK(isolate != NULL, "reflect_getType: NULL isolate");
    if (nargs < 1) {
        xr_log_warning("reflect", "Reflect.getType: requires 1 argument");
        return xr_null();
    }

    XrValue obj = args[0];
    XrayIsolate *X = isolate;

    XrClass *klass = NULL;
    if (xr_value_is_class(obj)) {
        klass = xr_value_to_class(obj);
    } else {
        klass = xr_value_get_class(X, obj);
    }

    if (!klass)
        return xr_null();

    XrTypeMetadata *meta = xr_registry_find_type_by_class(X, klass);
    if (!meta) {
        meta = xr_registry_register_class(X, klass);
        if (!meta) {
            xr_log_warning("reflect", "Reflect.getType: failed to register class '%s'",
                           xr_class_display_name(klass));
            return xr_null();
        }
    }

    return xr_create_type_object(X, meta);
}

XrValue xr_reflect_getTypeByName(XrayIsolate *isolate, XrValue self, XrValue *args, int nargs) {
    (void) self;
    XR_DCHECK(isolate != NULL, "reflect_getTypeByName: NULL isolate");
    if (nargs < 1) {
        xr_log_warning("reflect", "Reflect.getTypeByName: requires 1 argument");
        return xr_null();
    }

    if (!XR_IS_STRING(args[0])) {
        xr_log_warning("reflect", "Reflect.getTypeByName: argument must be string");
        return xr_null();
    }

    XrayIsolate *X = isolate;
    XrString *name_str = XR_TO_STRING(args[0]);
    const char *name = name_str->data;

    XrTypeMetadata *meta = xr_registry_find_type(X, name);
    if (!meta)
        return xr_null();

    return xr_create_type_object(X, meta);
}

XrValue xr_reflect_getAllTypes(XrayIsolate *isolate, XrValue self, XrValue *args, int nargs) {
    (void) self;
    XR_DCHECK(isolate != NULL, "reflect_getAllTypes: NULL isolate");
    (void) args;
    (void) nargs;

    XrayIsolate *X = isolate;

    int count = 0;
    XrTypeMetadata **all_types = xr_registry_get_all_types(X, &count);

    if (!all_types || count == 0) {
        XrArray *empty = xr_array_new(xr_current_coro(X));
        return xr_value_from_array(empty);
    }

    XrArray *array = xr_array_new(xr_current_coro(X));
    for (int i = 0; i < count; i++) {
        XrValue type_obj = xr_create_type_object(X, all_types[i]);
        xr_array_push(array, type_obj);
    }

    xr_free(all_types);
    return xr_value_from_array(array);
}

XrValue xr_reflect_isInstance(XrayIsolate *isolate, XrValue self, XrValue *args, int nargs) {
    (void) self;
    XR_DCHECK(isolate != NULL, "reflect_isInstance: NULL isolate");
    if (nargs < 2) {
        xr_log_warning("reflect", "Reflect.isInstance: requires 2 arguments");
        return xr_bool(false);
    }

    XrValue obj = args[0];
    XrValue type_obj = args[1];

    XrClass *obj_class = xr_value_get_class(isolate, obj);
    if (!obj_class)
        return xr_bool(false);

    XrTypeMetadata *type_meta = xr_get_type_metadata(type_obj);
    if (!type_meta || !type_meta->klass)
        return xr_bool(false);

    return xr_bool(xr_class_instanceof(obj_class, type_meta->klass));
}

XrValue xr_reflect_isInstanceOf(XrayIsolate *isolate, XrValue self, XrValue *args, int nargs) {
    (void) self;
    XR_DCHECK(isolate != NULL, "reflect_isInstanceOf: NULL isolate");
    if (nargs < 2) {
        xr_log_warning("reflect", "Reflect.isInstanceOf: requires 2 arguments");
        return xr_bool(false);
    }

    if (!XR_IS_STRING(args[1])) {
        xr_log_warning("reflect", "Reflect.isInstanceOf: second argument must be string");
        return xr_bool(false);
    }

    XrayIsolate *X = isolate;
    XrValue obj = args[0];
    XrString *type_name = XR_TO_STRING(args[1]);

    XrTypeMetadata *type_meta = xr_registry_find_type(X, type_name->data);
    if (!type_meta)
        return xr_bool(false);

    XrValue type_obj = xr_create_type_object(X, type_meta);
    XrValue new_args[2] = {obj, type_obj};
    return xr_reflect_isInstance(isolate, xr_null(), new_args, 2);
}

// Reflect.fieldCount(obj: Json): int
// Supports Json, struct (stack-allocated), and class instances
XrValue xr_reflect_fieldCount(XrayIsolate *isolate, XrValue self, XrValue *args, int nargs) {
    (void) self;
    if (nargs < 1)
        return xr_int(0);
    XrValue obj = args[0];

    if (xr_value_is_json(obj)) {
        XrJson *json = xr_value_to_json(obj);
        return xr_int((xr_Integer) xr_json_field_count(isolate, json));
    }

    // Struct ref (skip array_ref which uses ext for elem metadata)
    if (XR_IS_STRUCT_REF(obj) && !XR_IS_ARRAY_REF(obj)) {
        uint8_t *sptr = (uint8_t *) xr_to_struct_ptr(obj);
        XrClass *scls = *(XrClass **) sptr;
        if (scls && scls->struct_layout)
            return xr_int((xr_Integer) scls->struct_layout->field_count);
        if (scls)
            return xr_int((xr_Integer) xr_class_instance_field_count(scls));
    }

    // Class instance
    if (xr_value_is_instance(obj)) {
        XrInstance *inst = xr_value_to_instance(obj);
        XrClass *cls = xr_instance_get_class(inst);
        return xr_int((xr_Integer) xr_class_instance_field_count(cls));
    }

    return xr_int(0);
}

// Helper: convert tid to interned string value
static XrValue tid_to_string_value(XrayIsolate *isolate, uint8_t tid) {
    const char *name = (tid == 0) ? "any" : xr_typeid_name((XrTypeId) tid);
    size_t len = strlen(name);
    uint32_t hash = xr_string_hash(name, len);
    return XR_FROM_PTR(xr_string_intern(isolate, name, len, hash));
}

// Reflect.elementType(obj: Array|Set|Channel): string
XrValue xr_reflect_elementType(XrayIsolate *isolate, XrValue self, XrValue *args, int nargs) {
    (void) self;
    XR_DCHECK(isolate != NULL, "reflect_elementType: NULL isolate");
    if (nargs < 1)
        return tid_to_string_value(isolate, 0);
    XrValue obj = args[0];
    if (!XR_IS_PTR(obj))
        return tid_to_string_value(isolate, 0);
    uint8_t type = XR_HEAP_TYPE(obj);
    if (type == XR_TARRAY)
        return tid_to_string_value(isolate, XR_TO_ARRAY(obj)->elem_tid);
    if (type == XR_TSET)
        return tid_to_string_value(isolate, ((XrSet *) XR_VALUE_GCPTR(obj))->elem_tid);
    if (type == XR_TCHANNEL)
        return tid_to_string_value(isolate, ((XrChannel *) XR_VALUE_GCPTR(obj))->elem_tid);
    return tid_to_string_value(isolate, 0);
}

// Reflect.keyType(obj: Map): string
XrValue xr_reflect_keyType(XrayIsolate *isolate, XrValue self, XrValue *args, int nargs) {
    (void) self;
    XR_DCHECK(isolate != NULL, "reflect_keyType: NULL isolate");
    if (nargs < 1)
        return tid_to_string_value(isolate, 0);
    XrValue obj = args[0];
    if (XR_IS_PTR(obj) && XR_HEAP_TYPE(obj) == XR_TMAP) {
        XrMap *map = (XrMap *) XR_VALUE_GCPTR(obj);
        return tid_to_string_value(isolate, map->key_tid);
    }
    return tid_to_string_value(isolate, 0);
}

// Reflect.valueType(obj: Map): string
XrValue xr_reflect_valueType(XrayIsolate *isolate, XrValue self, XrValue *args, int nargs) {
    (void) self;
    XR_DCHECK(isolate != NULL, "reflect_valueType: NULL isolate");
    if (nargs < 1)
        return tid_to_string_value(isolate, 0);
    XrValue obj = args[0];
    if (XR_IS_PTR(obj) && XR_HEAP_TYPE(obj) == XR_TMAP) {
        XrMap *map = (XrMap *) XR_VALUE_GCPTR(obj);
        return tid_to_string_value(isolate, map->value_tid);
    }
    return tid_to_string_value(isolate, 0);
}

// Reflect.typeOf(obj: Json): string
// Returns full generic type string, e.g. "Array<int>", "Map<string, int>"
XrValue xr_reflect_typeOf(XrayIsolate *isolate, XrValue self, XrValue *args, int nargs) {
    (void) self;
    XR_DCHECK(isolate != NULL, "reflect_typeOf: NULL isolate");
    if (nargs < 1)
        return tid_to_string_value(isolate, 0);
    XrValue obj = args[0];

    // Non-pointer types: return base type name
    if (!XR_IS_PTR(obj)) {
        if (XR_IS_INT(obj))
            return tid_to_string_value(isolate, XR_TID_INT);
        if (XR_IS_FLOAT(obj))
            return tid_to_string_value(isolate, XR_TID_FLOAT);
        if (XR_IS_BOOL(obj))
            return tid_to_string_value(isolate, XR_TID_BOOL);
        if (XR_IS_NULL(obj))
            return tid_to_string_value(isolate, XR_TID_NULL);
        return tid_to_string_value(isolate, 0);
    }

    uint8_t heap_type = XR_HEAP_TYPE(obj);
    char buf[128];
    int n = 0;

    switch (heap_type) {
        case XR_TARRAY: {
            XrArray *arr = XR_TO_ARRAY(obj);
            if (arr->elem_tid != 0) {
                n = snprintf(buf, sizeof(buf), "Array<%s>",
                             xr_typeid_name((XrTypeId) arr->elem_tid));
            } else {
                n = snprintf(buf, sizeof(buf), "Array");
            }
            break;
        }
        case XR_TMAP: {
            XrMap *map = (XrMap *) XR_VALUE_GCPTR(obj);
            if (map->key_tid != 0 || map->value_tid != 0) {
                const char *kn = map->key_tid ? xr_typeid_name((XrTypeId) map->key_tid) : "unknown";
                const char *vn =
                    map->value_tid ? xr_typeid_name((XrTypeId) map->value_tid) : "unknown";
                n = snprintf(buf, sizeof(buf), "Map<%s, %s>", kn, vn);
            } else {
                n = snprintf(buf, sizeof(buf), "Map");
            }
            break;
        }
        case XR_TSET: {
            XrSet *set = (XrSet *) XR_VALUE_GCPTR(obj);
            if (set->elem_tid != 0) {
                n = snprintf(buf, sizeof(buf), "Set<%s>", xr_typeid_name((XrTypeId) set->elem_tid));
            } else {
                n = snprintf(buf, sizeof(buf), "Set");
            }
            break;
        }
        case XR_TCHANNEL: {
            XrChannel *ch = (XrChannel *) XR_VALUE_GCPTR(obj);
            if (ch->elem_tid != 0) {
                n = snprintf(buf, sizeof(buf), "Channel<%s>",
                             xr_typeid_name((XrTypeId) ch->elem_tid));
            } else {
                n = snprintf(buf, sizeof(buf), "Channel");
            }
            break;
        }
        case XR_TINSTANCE: {
            XrInstance *inst = (XrInstance *) XR_VALUE_GCPTR(obj);
            const char *dname = xr_class_display_name(inst->klass);
            uint8_t argc = inst->klass ? inst->klass->mono_type_argc : 0;
            const char **tnames = inst->klass ? inst->klass->mono_type_arg_names : NULL;
            if (argc > 0 && tnames) {
                char *ptr = buf;
                size_t rem = sizeof(buf);
                int w = snprintf(ptr, rem, "%s<", dname);
                ptr += w;
                rem -= (size_t) w;
                for (int i = 0; i < argc && rem > 2; i++) {
                    if (i > 0) {
                        w = snprintf(ptr, rem, ", ");
                        ptr += w;
                        rem -= (size_t) w;
                    }
                    w = snprintf(ptr, rem, "%s", tnames[i] ? tnames[i] : "unknown");
                    ptr += w;
                    rem -= (size_t) w;
                }
                snprintf(ptr, rem, ">");
                n = (int) (ptr - buf) + 1;
            } else {
                n = snprintf(buf, sizeof(buf), "%s", dname);
            }
            break;
        }
        case XR_TSTRING:
            n = snprintf(buf, sizeof(buf), "string");
            break;
        default: {
            XrTypeId tid = xr_value_typeid(obj);
            const char *name = xr_typeid_name(tid);
            n = snprintf(buf, sizeof(buf), "%s", name);
            break;
        }
    }

    if (n <= 0)
        return tid_to_string_value(isolate, 0);
    size_t len = (size_t) n;
    uint32_t hash = xr_string_hash(buf, len);
    return XR_FROM_PTR(xr_string_intern(isolate, buf, len, hash));
}

/* ========== Type Class Methods ========== */
